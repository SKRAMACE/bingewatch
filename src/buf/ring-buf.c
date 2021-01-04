#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#include "machine.h"
#include "filter.h"
#include "block-list-buffer.h"
#include "simple-buffers.h"

#define LOGEX_TAG "RING-BUF"
#include "logging.h"
#include "bw-log.h"

#define DEFAULT_BUF_BYTES 100*MB
#define DEFAULT_BLK_BYTES   10*MB
#define DEFAULT_ALIGN 1*MB
#define DEFAULT_REALLOC 16

static size_t default_buf_bytes = DEFAULT_BUF_BYTES;
static size_t default_blk_bytes = DEFAULT_BLK_BYTES;

const IOM *rb_machine;
static IOM *_ring_buffer_machine = NULL;

// Ring descriptor
struct ring_t {
    IO_DESC _b;  // Generic buffer

    size_t size;            // Total buffer capacity in bytes
    size_t bytes;           // Total stored data in bytes

    struct __block_t *wp;   // Pointer to next write-block
    pthread_mutex_t wlock;  // Write lock

    struct __block_t *rp;   // Pointer to next read-block
    pthread_mutex_t rlock;  // Read lock

    int flush;              // Flag used to keep reading available until the buffer is empty

    size_t block_size;      // Bytes per block
    size_t block_align;     // Block size alignment
    size_t block_realloc;   // Number of blocks to realloc
    size_t high_water_mark; // Upper limit for bytes
};

static pthread_mutex_t rb_machine_lock = PTHREAD_MUTEX_INITIALIZER;

static int
buf_read(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get ring from handle
    struct machine_desc_t *d = machine_get_desc(*handle);
    struct ring_t *ring = (struct ring_t *)d;
    if (!ring) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    char *data = IO_FILTER_ARGS_BUF; 

    // Lock reading from this buffer
    pthread_mutex_lock(&ring->rlock);
    struct __block_t *b = ring->rp;

    // Initialize read vars
    pthread_mutex_t *lock = &ring->_b.lock;
    int flush = ring->flush;
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

        memcpy(data, b->data, _bytes);
        remaining -= _bytes;
        data += _bytes;
        bytes_read += _bytes;

        if (partial_read) {
            memmove(b->data, b->data + _bytes, b->bytes - _bytes);
        }

        pthread_mutex_lock(lock);
        if (partial_read) {
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

    if (flush && bytes_read == 0) {
        io_desc_set_state(d, d->io_read, IO_DESC_DISABLING);
        return IO_COMPLETE;
    }

    return IO_SUCCESS;
}

static int
rb_data_init(struct ring_t *ring, size_t min_bytes)
{
    double n_d = (double)min_bytes / (double)ring->block_align;
    size_t n_i = (size_t)n_d;

    // Round up to the nearest 'aligned' byte count
    size_t block_size = (n_d != (double)n_i) ? (n_i + 1) * ring->block_align : n_i * ring->block_align;
        
    pthread_mutex_lock(&ring->wlock);
    pthread_mutex_lock(&ring->rlock);
    size_t bytes = block_data_fastalloc(ring->_b.pool, ring->wp, block_size);
    ring->block_size = block_size;
    ring->size += bytes;
    pthread_mutex_unlock(&ring->rlock);
    pthread_mutex_unlock(&ring->wlock);

    if (bytes == 0) {
        return IO_ERROR;
    }

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

    if (ring->size == 0) {
        trace("First write: allocating write buffers");
        if (rb_data_init(ring, *IO_FILTER_ARGS_BYTES) != IO_SUCCESS) {
            error("Failed to allocate write buffers");
            return IO_ERROR;
        }
    }

    char *data = IO_FILTER_ARGS_BUF; 

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
        char *tmp_f_buf = data;
        size_t tmp_r = _bytes;
        while (tmp_r > 0) {
            *tmp_b_data = *tmp_f_buf;
            tmp_b_data++;
            tmp_f_buf++;
            tmp_r--;
        }

        remaining -= _bytes;
        data += _bytes;
        written += _bytes;

        pthread_mutex_lock(lock);
        b->bytes = _bytes;
        struct __block_t *next = b->next;
        pthread_mutex_unlock(lock);


        // Expand the size of the ring if necessary
        if (!BLOCK_EMPTY(next)) {
            trace("Out of space: allocating more write buffers");

            pthread_mutex_lock(lock);
            ring->block_realloc *= 2;
            size_t n_blocks = ring->block_realloc;
            pthread_mutex_unlock(lock);

            struct __block_t *add_head = block_list_alloc(ring->_b.pool, n_blocks);
            struct __block_t *add_tail = add_head;
            while (add_tail->next) {
                add_tail = add_tail->next;
            }

            block_data_fastalloc(ring->_b.pool, add_head, ring->block_size);

            pthread_mutex_lock(lock);
            b->next = add_head;
            add_tail->next = next;
            pthread_mutex_unlock(lock);
        }

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

static IO_HANDLE
create_buffer(void *arg)
{
    IO_HANDLE h = 0;

    // Create a new pool for this buffer
    POOL *p = create_subpool(_ring_buffer_machine->alloc);
    if (!p) {
        error("Failed to create memory pool");
        return 0;
    }

    // Create a new buffer descriptor
    struct ring_t *ring = pcalloc(p, sizeof(struct ring_t));
    if (!ring) {
        error("Failed to allocate memory");
        goto free_and_return;
    }

    // Create block descriptors
    struct __block_t *blocks = block_list_alloc(p, DEFAULT_REALLOC);
    if (!blocks) {
        error("Failed to create descriptor");
        goto free_and_return;
    }

    forge_ring(blocks);
    ring->wp = blocks;
    ring->rp = blocks;
    ring->block_align = DEFAULT_ALIGN;
    ring->block_realloc = DEFAULT_REALLOC;

    pthread_mutex_init(&ring->wlock, NULL);
    pthread_mutex_init(&ring->rlock, NULL);

    if (machine_desc_init(p, _ring_buffer_machine, (IO_DESC *)ring) != IO_SUCCESS) {
        error("Failed to initialize mechine descriptor");
        goto free_and_return;
    }

    if (!filter_read_init(p, "ring_buf_r", buf_read, (IO_DESC *)ring)) {
        error("Failed to initialize read filter");
        goto free_and_return;
    }

    if (!filter_write_init(p, "ring_buf_w", buf_write, (IO_DESC *)ring)) {
        error("Failed to initialize write filter");
        goto free_and_return;
    }

    machine_register_desc((IO_DESC *)ring, &h);
    return h;

free_and_return:
    pfree(p);
    return h;
}

static void
stop_buffer(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
        return;
    }

    // Disable writing
    if (d->io_write) {
        io_desc_set_state(d, d->io_write, IO_DESC_DISABLING);
    }

    // Allow reading until the buffer is empty
    if (d->io_read) {
        struct ring_t *r = (struct ring_t *)d;
        r->flush = 1;
    }
}

static void *
get_metrics(IO_HANDLE h)
{
    struct ring_t *ring = (struct ring_t *)machine_get_desc(h);
    if (!ring) {
        error("Machine %d not found", h);
        return NULL;
    }

    return ring->_b.metrics;
}

/*
 * Mechanism for registering and accessing this io machine
 */
const IOM *
get_rb_machine()
{
    IOM *machine = _ring_buffer_machine;
    if (!machine) {
        machine = machine_register("ring_buffer");

        // Local Functions
        machine->create = create_buffer;
        machine->stop = stop_buffer;
        machine->destroy = destroy_rb_machine;
        machine->metrics = get_metrics;

        _ring_buffer_machine = machine;
        rb_machine = machine;
    }
    return (const IOM *)machine;
}

IO_HANDLE
new_rb_machine()
{
    const IOM *rb_machine = get_rb_machine();
    return rb_machine->create(NULL);
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

void
rb_set_alignment(IO_HANDLE h, uint32_t align)
{
    struct ring_t *ring = (struct ring_t *)machine_get_desc(h);
    if (!ring) {
        error("Machine %d not found", h);
    }

    ring->block_align = align;
}

void
rb_set_high_water_mark(IO_HANDLE h, size_t bytes)
{
    struct ring_t *ring = (struct ring_t *)machine_get_desc(h);
    if (!ring) {
        error("Machine %d not found", h);
    }

    ring->high_water_mark = bytes;
}

void
rb_set_log_level(char *level)
{
    blb_set_log_level(level);
    bw_set_log_level_str(level);
}
