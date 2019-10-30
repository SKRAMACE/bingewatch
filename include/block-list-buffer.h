#ifndef __BINGEWATCH_BLOCK_LIST_BUFFER_H__
#define __BINGEWATCH_BLOCK_LIST_BUFFER_H__

#include "machine.h"

#define BLOCK_FULL(b) (b->size == b->bytes)
#define BLOCK_EMPTY(b) (0 == b->bytes)

// Generic Block descriptor
struct __block_t {
    void *next;
    size_t size;
    size_t bytes;
    char *data;         // Data pointer
    void *__block_t_impl; // This is a placeholder so every implementation can be casted to this type
};

// Initialization Function Type
typedef void (*block_init)(struct __block_t *);
typedef void (*buffer_init)(IO_DESC *);

// Empty List Allocation
struct __block_t *block_list_alloc(POOL *p, size_t block_count);
struct __block_t *block_list_alloc_custom(POOL *p, size_t size_of, size_t block_count);

// List Data Allocation
int block_data_alloc(POOL *p, void *block, size_t bytes_per_block);
int block_data_fastalloc(POOL *p, void *block, size_t bytes_per_block);

// Buffer Management
void blb_lock(IO_HANDLE h);
void blb_unlock(IO_HANDLE h);
void forge_ring(void *blocklist);
int blb_init_struct(POOL *p, IO_DESC *b);

// Buffer Access
struct io_desc *blb_get_read_desc(IO_HANDLE h);
struct io_desc *blb_get_write_desc(IO_HANDLE h);
int blb_write(IO_HANDLE h, void *buf, size_t *len);
int blb_read(IO_HANDLE h, void *buf, size_t *len);
#endif
