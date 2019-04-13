#ifndef __BINGEWATCH_BLOCK_LIST_BUFFER_H__
#define __BINGEWATCH_BLOCK_LIST_BUFFER_H__

#include <radpool.h>

#define BLOCK_FULL(b) (b->size == b->bytes)
#define BLOCK_EMPTY(b) (0 == b->bytes)

// Generic Block descriptor
struct __block_t {
    void *next;
    uint64_t size;
    uint64_t bytes;
    char *data;         // Data pointer
    void *__block_t_impl; // This is a placeholder so every implementation can be casted to this type
};

// Generic buffer descriptor
struct __buffer_t {
    IO_HANDLE handle;         // IO Handle for this buffer
    void *next;               // Pointer to next buffer descriptor
    char in_use;              // Flag designates ring memory in use
    pthread_mutex_t lock;     // Mutex lock for this ring
    struct io_desc *io_read;  // IO read descriptor for this buffer
    struct io_desc *io_write; // IO write descriptor for this buffer
    POOL *pool;               // Memory management pool
    void *__buffer_impl;      // This is a placeholder so every implementation can be casted to this type
};

// Initialization Function Type
typedef void (*block_init)(struct __block_t *);
typedef void (*buffer_init)(struct __buffer_t *);

// Empty List Allocation
struct __block_t *block_list_alloc(POOL *p, uint64_t block_count);
struct __block_t *block_list_alloc_custom(POOL *p, size_t size_of, uint64_t block_count);

// List Data Allocation
int block_data_alloc(POOL *p, void *block, uint64_t bytes_per_block);
int block_data_fastalloc(POOL *p, void *block, uint64_t bytes_per_block);

// Buffer Management
void blb_add_buffer(struct __buffer_t *addme);
struct __buffer_t *blb_get_buffer(IO_HANDLE h);
void blb_lock(IO_HANDLE h);
void blb_unlock(IO_HANDLE h);
void blb_destroy(IO_HANDLE h);
void forge_ring(void *blocklist);

// Buffer Access
struct io_desc *blb_get_read_desc(IO_HANDLE h);
struct io_desc *blb_get_write_desc(IO_HANDLE h);
int blb_write(IO_HANDLE h, void *buf, uint64_t *len);
int blb_read(IO_HANDLE h, void *buf, uint64_t *len);
void blb_init_machine_functions(IOM *machine);
#endif
