#ifndef __IO_FILTER_H__
#define __IO_FILTER_H__

#include "machine.h"

#define IO_MAX_NAME_LEN 64

#define ERROR_DIRECTION() (printf("ERROR: Filter \"%s\" must be configures with a directon.\n", __FUNCTION__))

typedef enum io_blocking {
    IO_NO_BLOCK,
    IO_BLOCK,
} io_block_e;

enum io_filter_direction {
    IOF_BIDIRECTIONAL,
    IOF_READ,
    IOF_WRITE,
};

struct io_filter_t;
typedef int (*io_filter_fn)(struct io_filter_t*, void*, uint64_t*, io_block_e, int);

typedef struct io_filter_t {
    char enabled;
    char direction;
    char name[IO_MAX_NAME_LEN];
    void *alloc;

    // Implementation-specific
    void *obj;

    struct io_filter_t *next;
    io_filter_fn call;
} IO_FILTER;

#define IO_DEFAULT_ALIGN sizeof(char) 

// Standardize the naming convention for filter args
#define IO_FILTER_ARGS  struct io_filter_t *_iof_filter,\
                        void *_iof_buf,\
                        uint64_t *_iof_bytes,\
                        io_block_e _iof_block,\
                        int _iof_align

#define IO_FILTER_ARGS_FILTER _iof_filter
#define IO_FILTER_ARGS_BUF _iof_buf
#define IO_FILTER_ARGS_BYTES _iof_bytes
#define IO_FILTER_ARGS_BLOCK _iof_block
#define IO_FILTER_ARGS_ALIGN _iof_align

#define PASS_IO_FILTER_ARGS _iof_filter, _iof_buf, _iof_bytes, _iof_block, _iof_align

// If the filter is disabled, skip to next filter
#define IOF_DISABLED() if (!_iof_filter->enabled) { return CALL_NEXT_FILTER(); }

// Call the next filter using standard FILTER_ARGS
#define CALL_NEXT_FILTER() _iof_filter->next->call(_iof_filter->next, _iof_buf, _iof_bytes, _iof_block, _iof_align)

// Call the next filter using standard FILTER_ARGS
#define CALL_NEXT_FILTER_ALIGNED(x) _iof_filter->next->call(_iof_filter->next, _iof_buf, _iof_bytes, _iof_block, x)

// Call the next filter with new buffer
#define CALL_NEXT_FILTER_BUF(buf, len) _iof_filter->next->call(_iof_filter->next, buf, len, _iof_block, _iof_align)

// Call the next filter with custom args
#define CALL_NEXT_FILTER_ARGS(...) _iof_filter->next->call(_iof_filter->next, __VA_ARGS__)

struct io_filter_t *create_filter(void *alloc, const char *name, io_filter_fn fn);
struct io_filter_t *get_io_filter(struct io_filter_t *filter, const char *name);
void io_filter_enable(struct io_filter_t *filter, const char *name);
void io_filter_disable(struct io_filter_t *filter, const char *name);
void register_write_filter(int h, io_filter_fn fn, const char *name);
void register_read_filter(int h, io_filter_fn fn, const char *name);
void add_write_filter(int h, struct io_filter_t *addme);
void add_read_filter(int h, struct io_filter_t *addme);
struct io_filter_t *filter_read_init(POOL *p, const char *name, io_filter_fn fn, IO_DESC *d);
struct io_filter_t *filter_write_init(POOL *p, const char *name, io_filter_fn fn, IO_DESC *d);

#endif
