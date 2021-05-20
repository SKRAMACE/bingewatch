#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/sysinfo.h>

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
    if (!block) {
        return 0;
    }

    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        warn("sysinfo failed: Not fast-allocating block buffer");
        return 0;
    }

    if (bytes_per_block > info.freeram) {
        warn("Insufficient memory: Not fast-allocating block buffer");
        return 0;
    }

    int n_blocks = 0;

    struct __block_t *b = (struct __block_t *)block;
    do {
        int n = n_blocks + 1;
        if (n * bytes_per_block < info.freeram) {
            b->data = palloc(p, bytes_per_block);
            b->bytes = 0;
            b->size = bytes_per_block;
            n_blocks++;
        }
        b = b->next;
    } while (b && (b != block));

    // Print Trace
    char full_bytestr[64];
    char block_bytestr[64];
    size_t_fmt(full_bytestr, 64, n_blocks * bytes_per_block);
    size_t_fmt(block_bytestr, 64, bytes_per_block);
    trace("Allocating %sB (%sB * %d blocks)", full_bytestr, block_bytestr, n_blocks);

    struct __block_t *final_next = b;
    b = (struct __block_t *)block;
    b[n_blocks - 1].next = final_next;

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

    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        warn("sysinfo failed: Not fast-allocating block buffer");
        return 0;
    }

    if (bytes_per_block > info.freeram) {
        warn("Insufficient memory: Not fast-allocating block buffer");
        return 0;
    }

    // Calculate size
    struct __block_t *b = (struct __block_t *)block;
    int n_blocks = 0;
    do {
        int n = n_blocks + 1;
        if (n * bytes_per_block < info.freeram) {
            n_blocks++;
        }
        b = b->next;
    } while (b && (b != block));

    // Trace Printing
    char full_bytestr[64];
    char block_bytestr[64];
    size_t_fmt(full_bytestr, 64, n_blocks * bytes_per_block);
    size_t_fmt(block_bytestr, 64, bytes_per_block);
    trace("Fast-allocating %sB (%sB * %d blocks)", full_bytestr, block_bytestr, n_blocks);

    // Allocate memory
    size_t bytes = n_blocks * bytes_per_block;
    char *buf = palloc(p, bytes);
    if (!buf) {
        error("Failed to allocate bytes for block buffer", bytes);
        return 0;
    }

    // Partition buffer into blocks
    struct __block_t *final_next = b;
    b = (struct __block_t *)block;
    for (int n = 0; n < n_blocks; n++) {
        b->data = buf;
        b->bytes = 0;
        b->size = bytes_per_block;
        buf += bytes_per_block;

        if (n_blocks - n > 1) {
            b = b->next;
        } else {
            b->next = final_next;
        }
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

struct blb_rw_t *
blb_init_rw(POOL *pool, size_t bytes_per_block, size_t n_blocks)
{
    // Init BLB
    struct blb_rw_t *ret = palloc(pool, sizeof(struct blb_rw_t));
    ret->buf = block_list_alloc(pool, n_blocks);
    block_data_fastalloc(pool, ret->buf, bytes_per_block);
    forge_ring(ret->buf);

    // Set read and write pointers
    ret->wp = ret->buf;
    pthread_mutex_init(&ret->wlock, NULL);

    ret->rp = ret->buf;
    pthread_mutex_init(&ret->rlock, NULL);

    return ret;
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
blb_empty(struct __block_t *blb)
{
    struct __block_t *b = blb;
    do {
        b->bytes = 0;
        b = b->next;
        b->state = BLB_STATE_NORMAL;
    } while (b && b != blb);
}

void
blb_rw_empty(struct blb_rw_t *rw)
{
    if (!rw) {
        return;
    }

    pthread_mutex_lock(&rw->rlock);
    pthread_mutex_lock(&rw->wlock);

    blb_empty(rw->buf);
    rw->rp = rw->buf;
    rw->wp = rw->buf;

    pthread_mutex_unlock(&rw->rlock);
    pthread_mutex_unlock(&rw->wlock);
}

void
blb_rw_get_bytes(struct blb_rw_t *rw, size_t *size, size_t *bytes)
{
    if (!rw) {
        *size = 0;
        *bytes = 0;
        return;
    }

    pthread_mutex_lock(&rw->rlock);
    pthread_mutex_lock(&rw->wlock);

    size_t _size = 0;
    size_t _bytes = 0;

    struct __block_t *b = rw->buf;
    do {
        _size += b->size;
        _bytes += b->bytes;
        b = b->next;
    } while (b && b != rw->buf);

    pthread_mutex_unlock(&rw->rlock);
    pthread_mutex_unlock(&rw->wlock);

    *bytes = _bytes;
    *size = _size;
}

void
blb_set_log_level(char *level)
{
    bw_set_log_level_str(level);
}
