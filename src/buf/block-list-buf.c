#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>

#include "machine.h"
#include "filter.h"
#include "block-list-buffer.h"
#include "bw-util.h"

#define LOGEX_TAG "BL-BUF"
#include "logging.h"
#include "bw-log.h"

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
            printf("Failed to allocate %zu bytes for block descriptor\n", size_of);
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
size_t
block_data_alloc(POOL *p, void *block, size_t bytes_per_block) {
    int n_blocks = 0;

    struct __block_t *b = (struct __block_t *)block;
    do {
        b->data = palloc(p, bytes_per_block);
        b->bytes = 0;
        b->size = bytes_per_block;

        n_blocks++;
        b = b->next;
    } while (b && (b != block));

    char full_bytestr[64];
    char block_bytestr[64];
    size_t_fmt(full_bytestr, 64, n_blocks * bytes_per_block);
    size_t_fmt(block_bytestr, 64, bytes_per_block);
    trace("Allocating %sB (%sB * %d blocks)", full_bytestr, block_bytestr, n_blocks);
    return n_blocks * bytes_per_block;
}

/*
 * Allocate a large chunk of memory and partition for each block
 */
size_t
block_data_fastalloc(POOL *p, void *block, size_t bytes_per_block) {
    if (!block) {
        return 0;
    }

    // Calculate size
    struct __block_t *b = (struct __block_t *)block;
    int n_blocks = 0;
    do {
        n_blocks++;
        b = b->next;
    } while (b && (b != block));

    char full_bytestr[64];
    char block_bytestr[64];
    size_t_fmt(full_bytestr, 64, n_blocks * bytes_per_block);
    size_t_fmt(block_bytestr, 64, bytes_per_block);
    size_t bytes = n_blocks * bytes_per_block;
    trace("Fast-allocating %sB (%sB * %d blocks)", full_bytestr, block_bytestr, n_blocks);

    // Allocate memory
    char *buf = palloc(p, bytes);
    if (!buf) {
        error("Failed to allocate bytes for block buffer", bytes);
        return 0;
    }

    // Partition buffer into blocks
    b = (struct __block_t *)block;
    while (b && !b->data) {
        b->data = buf;
        b->bytes = 0;
        b->size = bytes_per_block;
        buf += bytes_per_block;
        b = b->next;
    }

    return bytes;
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

void
blb_set_log_level(char *level)
{
    bw_set_log_level_str(level);
}
