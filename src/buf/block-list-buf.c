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
static pthread_mutex_t buffer_list_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Returns a linked list of empty, custom-sized block descriptors
 */
struct __block_t *
block_list_alloc_custom(POOL *p, size_t size_of, size_t block_count)
{
    struct __block_t *blocks = NULL;
    struct __block_t *b = NULL;

    if (0 == block_count) {
        printf("%s: WARNING: Block count is zero\n", __FUNCTION__);
        return NULL;
    }

    size_t remaining = block_count;
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
block_list_alloc(POOL *p, size_t block_count) {
    return block_list_alloc_custom(p, sizeof(struct __block_t), block_count);
}

/*
 * Allocate a fixed number of bytes for each block
 */
int
block_data_alloc(POOL *p, void *block, size_t bytes_per_block) {
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
block_data_fastalloc(POOL *p, void *block, size_t bytes_per_block) {
    // Count bytes
    struct __block_t *b = (struct __block_t *)block;
    size_t total_bytes = 0;
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

int
blb_init_struct(POOL *p, IO_DESC *b)
{
    pthread_mutex_init(&b->lock, NULL);
    b->pool = p;

    b->io_read = (struct io_desc *)pcalloc(p, sizeof(struct io_desc));
    if (!b->io_read) {
        printf("Failed to initialize read descriptor\n");
        return IO_ERROR;
    }
    b->io_read->alloc = p;

    b->io_write = (struct io_desc *)pcalloc(p, sizeof(struct io_desc));
    if (!b->io_write) {
        printf("Failed to initialize write descriptor\n");
        return IO_ERROR;
    }
    b->io_write->alloc = p;

    return IO_SUCCESS;
}
