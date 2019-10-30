#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#include "radpool.h"
#include "machine.h"
#include "filter.h"
#include "block-list-buffer.h"
#include "simple-buffers.h"

#define DEFAULT_BUF_BYTES 100*MB
#define DEFAULT_BLK_BYTES   1*MB
#define DEFAULT_ALIGN 4

static size_t default_buf_bytes = DEFAULT_BUF_BYTES;
static size_t default_blk_bytes = DEFAULT_BLK_BYTES;
static uint16_t default_align = DEFAULT_ALIGN;

static IOM *ring_buffer_machine = NULL;

// Ring descriptor
struct ring_t {
    IO_DESC _b;  // Generic buffer

    size_t size;         // Total capacity in all blocks (in bytes)
    size_t bytes;        // Total data written in all blocks (in bytes)
    struct __block_t *wp;  // Pointer to next write (empty) block
    struct __block_t *rp;  // Pointer to next read (filled) block
    pthread_mutex_t wlock; // Mutex lock for writing to this ring
    pthread_mutex_t rlock; // Mutex lock for reading to this ring
    size_t block_size;   // Bytes per block
};

static pthread_mutex_t rb_machine_lock = PTHREAD_MUTEX_INITIALIZER;

static int
buf_read(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get ring from handle
    struct ring_t *ring = (struct ring_t *)machine_get_desc(*handle);
    if (!ring) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Lock reading from this buffer
    pthread_mutex_lock(&ring->rlock);
    struct __block_t *b = ring->rp;

    // Initialize read vars
    pthread_mutex_t *lock = &ring->_b.lock;
    size_t bytes_read = 0;
    size_t remaining = *IO_FILTER_ARGS_BYTES;

    while (remaining % IO_FILTER_ARGS_ALIGN != 0) {
        remaining--;
    }

    while (remaining) {
        size_t _bytes = b->bytes;

        if (0 == _bytes) {
            if (IO_FILTER_ARGS_BLOCK == IO_BLOCK) {
                continue;
            }
            if (bytes_read % IO_FILTER_ARGS_ALIGN != 0) {
                continue;
            }
            break;
        }

        int partial_read = (remaining < _bytes) ? 1 : 0;
        if (partial_read) {
            _bytes = remaining;
        }

        memcpy(IO_FILTER_ARGS_BUF, b->data, _bytes);
        remaining -= _bytes;
        IO_FILTER_ARGS_BUF += _bytes;
        bytes_read += _bytes;

        pthread_mutex_lock(lock);
        if (partial_read) {
            memmove(b->data, b->data + _bytes, b->bytes - _bytes);
            b->bytes -= _bytes;
        } else {
            b->bytes -= _bytes;
            b = b->next;
        }
        pthread_mutex_unlock(lock);
    }

    pthread_mutex_lock(lock);
    ring->bytes -= bytes_read;
    pthread_mutex_unlock(lock);

    // Unlock reading from this buffer
    ring->rp = b;
    pthread_mutex_unlock(&ring->rlock);

    *IO_FILTER_ARGS_BYTES = bytes_read;
    return IO_SUCCESS;
}

// Write to a buffer
static int
buf_write(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get ring from handle
    struct ring_t *ring = (struct ring_t *)machine_get_desc(*handle);
    if (!ring) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Lock writing to this buffer
    pthread_mutex_lock(&ring->wlock);
    struct __block_t *b = ring->wp;

    // Initialize write vars
    pthread_mutex_t *lock = &ring->_b.lock;
    size_t written = 0;
    size_t remaining = *IO_FILTER_ARGS_BYTES;

    // Write input bytes to buffer
    while (remaining) {
        size_t _bytes = (remaining < b->size) ? remaining : b->size;

        char *tmp_b_data = b->data;
        char *tmp_f_buf = IO_FILTER_ARGS_BUF;
        size_t tmp_r = _bytes;
        while (tmp_r > 0) {
            *tmp_b_data = *tmp_f_buf;
            tmp_b_data++;
            tmp_f_buf++;
            tmp_r--;
        }

        //memcpy(b->data, IO_FILTER_ARGS_BUF, _bytes);
        remaining -= _bytes;
        IO_FILTER_ARGS_BUF += _bytes;
        written += _bytes;

        pthread_mutex_lock(lock);
        b->bytes = _bytes;

        // Expand the size of the ring if necessary
        struct __block_t *next = b->next;
        if (!BLOCK_EMPTY(next)) {
            // Add enough space + 1 extra block
            size_t block_count = (remaining / ring->block_size) + 1;

            struct __block_t *add = block_list_alloc(ring->_b.pool, block_count);
            block_data_fastalloc(ring->_b.pool, add, ring->block_size);

            // Link head of new block segment into ring
            b->next = add;

            // Get last block
            while (add->next) {
                add = add->next;
            }

            // Link tail of new block segment into ring
            add->next = next;
        }
        pthread_mutex_unlock(lock);

        b = b->next;
    }

    pthread_mutex_lock(lock);
    ring->bytes += written;
    pthread_mutex_unlock(lock);

    ring->wp = b;

    // Unlock writing to this buffer
    pthread_mutex_unlock(&ring->wlock);

    *IO_FILTER_ARGS_BYTES = written;
    return IO_SUCCESS;
}

/*
 * Create/destroy ring buffers
 */
static void
destroy_rb_machine(IO_HANDLE h)
{
    printf("Destroying buffer %d\n", h);
    machine_destroy_desc(h);
}

static inline void
sanitize_args(struct rbiom_args *args)
{
    pthread_mutex_lock(&rb_machine_lock);
    // Set null values to defaults
    if (0 == args->buf_bytes) {
        //printf("Using default buffer size: %zu\n", default_buf_bytes);
        args->buf_bytes = default_buf_bytes;
    }
    if (0 == args->block_bytes) {
        //printf("Using default block size: %zu\n", default_blk_bytes);
        args->block_bytes = default_blk_bytes;
    }
    if (0 == args->align) {
        //printf("Using default alignment: %#x\n", default_align);
        args->align = default_align;
    }

    // Verify that alignment is a power of 2
    uint16_t i = 15;
    while (1) {
        if ((1 << i) == args->align) {
            break;
        }

        if (i > 0) {
            i--;
            continue;
        }

        //printf("Invalid alignment, using default: %#x\n", default_align);
        args->align = default_align;
        break;
    }

    // Verify that block bytes are properly aligned
    
    size_t a = (size_t)args->align;
    size_t adjust = (a - (args->block_bytes & (a - 1))) & (a - 1);
    if (adjust > 0) {
        //printf("Invalid block alignment, adjusting from %zu to %zu\n", args->block_bytes, args->block_bytes + adjust);
        args->block_bytes += adjust;
    }
    pthread_mutex_unlock(&rb_machine_lock);
}

#define MACHINE_CRATE_IMPL() \
    POOL *p = create_subpool(alloc);\
    if (!p) { printf("ERROR: Filed to create memory pool\n"); return 0; }\

static IO_HANDLE
create_buffer(void *arg)
{
    // Create a new pool for this buffer
    POOL *p = create_subpool(ring_buffer_machine->alloc);
    if (!p) {
        printf("ERROR: Failed to create memory pool\n");
        return 0;
    }

    // Create a new buffer descriptor
    struct ring_t *ring = pcalloc(p, sizeof(struct ring_t));
    if (!ring) {
        printf("ERROR: Failed to allocate %#zx bytes for ring descriptor\n", sizeof(struct ring_t));
        pfree(p);
        return 0;
    }

    // Block arithmetic
    struct rbiom_args *args = (struct rbiom_args *)arg;
    sanitize_args(args);

    size_t bytes = args->buf_bytes;
    size_t block_size = args->block_bytes;
    size_t block_count = bytes / block_size;
    if ((block_count * block_size) < bytes) {
        block_count++;
    }

    // Create block descriptors
    struct __block_t *blocks = block_list_alloc(p, block_count);
    if (!blocks) {
        printf("ERROR: Failed to create new buffer\n");
        pfree(p);
        return 0;
    }

    // Create block data
    if (block_data_fastalloc(p, blocks, block_size) != IO_SUCCESS) {
        printf("ERROR: Failed to create new buffer\n");
        pfree(p);
        return 0;
    }

    forge_ring(blocks);

    ring->block_size = block_size;
    ring->wp = blocks;
    ring->rp = blocks;
    pthread_mutex_init(&ring->wlock, NULL);
    pthread_mutex_init(&ring->rlock, NULL);

    if (machine_desc_init(p, ring_buffer_machine, (IO_DESC *)ring) != IO_SUCCESS) {
        pfree(p);
        return 0;
    }

    if (!filter_read_init(p, "_buf", buf_read, (IO_DESC *)ring)) {
        printf("ERROR: Failed to initialize read filter\n");
        pfree(p);
        return 0;
    }

    if (!filter_write_init(p, "_buf", buf_write, (IO_DESC *)ring)) {
        printf("ERROR: Failed to initialize write filter\n");
        pfree(p);
        return 0;
    }

    IO_HANDLE h;
    machine_register_desc((IO_DESC *)ring, &h);

    return h;
}

void
rbiom_update_defaults(struct rbiom_args *rb)
{
    sanitize_args(rb);

    pthread_mutex_lock(&rb_machine_lock);
    default_buf_bytes = rb->buf_bytes;
    default_blk_bytes = rb->block_bytes;
    default_align = rb->align;
    pthread_mutex_unlock(&rb_machine_lock);
}

/*
 * Mechanism for registering and accessing this io machine
 */
const IOM *
get_rb_machine()
{
    IOM *machine = ring_buffer_machine;
    if (!machine) {
        machine = machine_register("ring_buffer");

        // Local Functions
        machine->create = create_buffer;
        machine->destroy = destroy_rb_machine;

        ring_buffer_machine = machine;
    }
    return (const IOM *)machine;
}

const IOM *
new_rb_machine(IO_HANDLE *h, size_t buffer_size, size_t block_size)
{
    const IOM *rb_machine = get_rb_machine();

    struct rbiom_args rb_args = {buffer_size, block_size};
    *h = rb_machine->create(&rb_args);
    return rb_machine;
}

size_t
rb_get_size(IO_HANDLE h)
{
    struct ring_t *ring = (struct ring_t *)machine_get_desc(h);
    if (!ring) {
        return 0;
    }
    return ring->size;
}

size_t
rb_get_bytes(IO_HANDLE h)
{
    struct ring_t *ring = (struct ring_t *)machine_get_desc(h);
    if (!ring) {
        return 0;
    }
    return ring->bytes;
}
