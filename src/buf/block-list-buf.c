#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>

#include "radpool.h"
#include "machine.h"
#include "filter.h"
#include "block-list-buffer.h"

// Holds all of the __buffer structs
static struct __buffer_t *buffers = NULL;
static pthread_mutex_t buffer_list_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Returns a linked list of empty, custom-sized block descriptors
 */
struct __block_t *
block_list_alloc_custom(POOL *p, size_t size_of, uint64_t block_count)
{
    struct __block_t *blocks = NULL;
    struct __block_t *b = NULL;

    if (0 == block_count) {
        printf("%s: WARNING: Block count is zero\n", __FUNCTION__);
        return NULL;
    }

    uint32_t remaining = block_count;
    while (remaining--) {
        struct __block_t *new = pcalloc(p, size_of);
        if (!new) {
            printf("Failed to allocate %" PRIu64 " bytes for block descriptor\n", size_of);
            return NULL;
        }

        if (!b) {
            blocks = new;
        } else {
            b->next = new;
        }
        b = new;
    }

    return blocks;
}

/*
 * Returns a linked list of empty block descriptors
 */
struct __block_t *
block_list_alloc(POOL *p, uint64_t block_count) {
    return block_list_alloc_custom(p, sizeof(struct __block_t), block_count);
}

/*
 * Allocate a fixed number of bytes for each block
 */
int
block_data_alloc(POOL *p, void *block, uint64_t bytes_per_block) {
    struct __block_t *b = (struct __block_t *)block;
    while (b) {
        b->data = palloc(p, bytes_per_block);
        b->bytes = 0;
        b->size = bytes_per_block;
        if (!b->data) {
            printf("Failed to allocate %" PRIu64 " bytes for block data\n", bytes_per_block);
            return IO_ERROR;
        }
        b = b->next;
    }

    return IO_SUCCESS;
}

/*
 * Allocate a large chunk of memory and partition for each block
 */
int
block_data_fastalloc(POOL *p, void *block, uint64_t bytes_per_block) {
    // Count bytes
    struct __block_t *b = (struct __block_t *)block;
    uint64_t total_bytes = 0;
    while (b) {
        total_bytes += bytes_per_block;
        b = b->next;
    }

    // One alloc (hence "fastalloc")
    char *buf = palloc(p, total_bytes);
    if (!buf) {
        printf("Failed to allocate %" PRIu64 " bytes for block data\n", total_bytes);
        return IO_ERROR;
    }

    // Partition buffer into blocks
    b = (struct __block_t *)block;
    while (b) {
        b->data = buf;
        b->bytes = 0;
        b->size = bytes_per_block;
        buf += bytes_per_block;
        b = b->next;
    }

    return IO_SUCCESS;
}

void
forge_ring(void *blocklist)
{
    struct __block_t *blocks = (struct __block_t *)blocklist;
    struct __block_t *first = blocks;

    while (blocks->next) {
        // Avoid infinite loop if list is already a ring
        if (blocks->next == first) {
            return;
        }
        blocks = blocks->next;
    }
    blocks->next = first;
}

static inline void
acquire_buffer(struct __buffer_t *b)
{
    pthread_mutex_lock(&b->lock);
    b->in_use++;
    pthread_mutex_unlock(&b->lock);
}

static inline void
release_buffer(struct __buffer_t *b)
{
    pthread_mutex_lock(&b->lock);
    b->in_use--;
    pthread_mutex_unlock(&b->lock);
}

// Add a new ring descriptor
void
blb_add_buffer(struct __buffer_t *addme)
{
    pthread_mutex_lock(&buffer_list_lock);
    struct __buffer_t *b = buffers;
    if (!b) {
        buffers = addme;
    } else {
        while (b->next) {
            b = b->next;
        }
        b->next = addme;
    }
    pthread_mutex_unlock(&buffer_list_lock);
}

// Get a pointer to a ring with the specified handle
struct __buffer_t *
blb_get_buffer(IO_HANDLE h)
{
    pthread_mutex_lock(&buffer_list_lock);
    struct __buffer_t *b = buffers;

    while (b) {
        if (b->handle == h) {
            break;
        }

        b = b->next;
    }
    pthread_mutex_unlock(&buffer_list_lock);

    return b;
}

static void
free_buffer(struct __buffer_t *b) {
    if (!b) {
        return;
    }

    pthread_mutex_destroy(&b->lock);
    pfree(b->pool);
}

// Free the ring descriptor and associated block ring
void
blb_destroy(IO_HANDLE h)
{
    pthread_mutex_lock(&buffer_list_lock);
    struct __buffer_t *b = buffers;

    struct __buffer_t *bp = NULL;
    while (b) {
        if (b->handle == h) {
            break;
        }
        bp = b;

        b = b->next;
    }

    // handle not found
    if (!b) {
        pthread_mutex_unlock(&buffer_list_lock);
        return;

    // handle in first slot
    } else if (!bp) {
        buffers = b->next;

    } else if (b && bp) {
        bp->next = b->next;
    }
    pthread_mutex_unlock(&buffer_list_lock);

    while (b->in_use) {
        usleep(500000);
        continue;
    }

    pthread_mutex_lock(&buffer_list_lock);
    free_buffer(b);
    pthread_mutex_unlock(&buffer_list_lock);
}

/*
 * Lock/Unlock ring for external configuration
 */
void
blb_lock(IO_HANDLE h)
{
    struct __buffer_t *b = blb_get_buffer(h);
    if (!b) {
        return;
    }

    pthread_mutex_lock(&b->lock);
}

void
blb_unlock(IO_HANDLE h)
{
    struct __buffer_t *b = blb_get_buffer(h);
    if (!b) {
        return;
    }

    pthread_mutex_unlock(&b->lock);
}

/*
 * Return a pointer to the first read descriptor
 */
struct io_desc *
blb_get_read_desc(IO_HANDLE h)
{
    struct __buffer_t *b = blb_get_buffer(h);
    if (!b) {
        return NULL;
    }

    return b->io_read;
}

/*
 * Return a pointer to the first write descriptor
 */
struct io_desc *
blb_get_write_desc(IO_HANDLE h)
{
    struct __buffer_t *b = blb_get_buffer(h);
    if (!b) {
        return NULL;
    }

    return b->io_write;
}

/*
 * Copy "len" bytes from ring with handle==h to "buf"
 */
int
blb_read(IO_HANDLE h, void *buf, uint64_t *len)
{
    // Return NULL if ring doesn't exist
    struct __buffer_t *b = blb_get_buffer(h);
    if (!b) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_buffer(b);
    struct io_filter_t *f = (struct io_filter_t *)b->io_read->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_buffer(b);

    return status;
}

/*
 * Copy "len" bytes from "buf" to ring with handle==h
 */
int
blb_write(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct __buffer_t *b = blb_get_buffer(h);
    if (!b) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_buffer(b);
    struct io_filter_t *f = (struct io_filter_t *)b->io_write->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_buffer(b);

    return status;
}

void
blb_init_machine_functions(IOM *machine)
{
    machine->lock = blb_lock;
    machine->unlock = blb_unlock;
    machine->get_read_desc = blb_get_read_desc;
    machine->get_write_desc = blb_get_write_desc;
    machine->read = blb_read;
    machine->write = blb_write;
}
