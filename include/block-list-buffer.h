#ifndef __BINGEWATCH_BLOCK_LIST_BUFFER_H__
#define __BINGEWATCH_BLOCK_LIST_BUFFER_H__

#include "machine.h"

#define BLOCK_FULL(b) (b->size == b->bytes)
#define BLOCK_EMPTY(b) (0 == b->bytes)

enum block_state_e {
    BLB_STATE_NORMAL,
    BLB_STATE_DELETE,
};

// Generic Block descriptor
struct __block_t {
    // Pointer to next block
    void *next;

    // Pointer to the block data buffer
    char *data;

    // Size of the block data in bytes
    size_t size;

    // Number of bytes in use
    size_t bytes;

    // Block state
    enum block_state_e state;

    // Placeholder for extension
    void *__block_t_impl;
};

struct blb_rw_t {
    struct __block_t *buf;

    struct __block_t *wp;   // Pointer to next write-block
    pthread_mutex_t wlock;  // Write lock

    struct __block_t *rp;   // Pointer to next read-block
    pthread_mutex_t rlock;  // Read lock
};

// Initialization Function Type
typedef void (*block_init)(struct __block_t *);
typedef void (*buffer_init)(IO_DESC *);

// Empty List Allocation
struct __block_t *block_list_alloc(POOL *p, size_t block_count);
struct __block_t *block_list_alloc_custom(POOL *p, size_t size_of, size_t block_count);

// List Data Allocation
size_t block_data_alloc(POOL *p, void *block, size_t bytes_per_block);
size_t block_data_fastalloc(POOL *p, void *block, size_t bytes_per_block);

// Buffer Management
struct blb_rw_t *blb_init_rw(POOL *pool, size_t bytes_per_block, size_t n_blocks);
void blb_lock(IO_HANDLE h);
void blb_unlock(IO_HANDLE h);
void forge_ring(void *blocklist);
int blb_init_struct(POOL *p, IO_DESC *b);
void blb_empty(struct __block_t *blb);
void blb_rw_empty(struct blb_rw_t *rw);

// Buffer Access
struct io_desc *blb_get_read_desc(IO_HANDLE h);
struct io_desc *blb_get_write_desc(IO_HANDLE h);
int blb_write(IO_HANDLE h, void *buf, size_t *len);
int blb_read(IO_HANDLE h, void *buf, size_t *len);
#endif
