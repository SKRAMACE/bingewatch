#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "machine.h"
#include "segment.h"
#include "filter.h"

struct generic_counter_t {
    size_t total;
    size_t limit;
    size_t period;
};

static int
byte_count_limiter(IO_FILTER_ARGS)
{
    IOF_DISABLED();
    int ret = IO_ERROR;

    enum io_filter_direction dir = IO_FILTER_ARGS_FILTER->direction;
    struct generic_counter_t *limit = (struct generic_counter_t *)IO_FILTER_ARGS_FILTER->obj;

    if (dir == IOF_READ) { 
        ret = CALL_NEXT_FILTER();
        if (ret != IO_SUCCESS) {
            return ret;
        }
    }

    size_t bytes = *IO_FILTER_ARGS_BYTES;

    if (limit->total + bytes <= limit->limit) {
        limit->total += bytes;

    } else {
        bytes = limit->limit - limit->total;
        limit->total += bytes;
    }

    if (dir == IOF_WRITE) {
        *IO_FILTER_ARGS_BYTES = bytes;
        ret = CALL_NEXT_FILTER();
    }

    if (limit->total >= limit->limit) {
        ret = (ret == IO_SUCCESS) ? IO_COMPLETE : ret;
    }

    return ret;
}

struct io_filter_t *
create_byte_count_limit_filter(void *alloc, const char *name, size_t byte_limit)
{
    struct io_filter_t *f = create_filter(alloc, name, byte_count_limiter);
    struct generic_counter_t *limit = palloc(alloc, sizeof(struct generic_counter_t));
    limit->total = 0;
    limit->limit = byte_limit;
    f->obj = limit;
    return f;
}

int
byte_counter(IO_FILTER_ARGS)
{
    IOF_DISABLED();
    int ret = CALL_NEXT_FILTER();

    // If initialized improperly, just return status
    if (!IO_FILTER_ARGS_FILTER->obj) {
        return ret;
    }

    // Access internal struct
    struct generic_counter_t *limit = (struct generic_counter_t *)IO_FILTER_ARGS_FILTER->obj;

    // Count bytes, and return COMPLETE if the limit is reached
    limit->total += *IO_FILTER_ARGS_BYTES;
    if (limit->total >= limit->limit) {
        printf("%zu bytes\n", limit->total);
        limit->limit += limit->period;
    }

    return ret;
}

struct io_filter_t *
create_byte_counter_filter(void *alloc, const char *name, size_t bytes_per_sample)
{
    struct io_filter_t *f = create_filter(alloc, name, byte_counter);
    struct generic_counter_t *limit = palloc(alloc, sizeof(struct generic_counter_t));
    limit->total = 0;
    limit->period = bytes_per_sample;
    limit->limit = bytes_per_sample;
    f->obj = limit;
    return f;
}
