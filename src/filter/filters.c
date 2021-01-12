#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>

#include "machine.h"
#include "segment.h"
#include "filter.h"

enum time_limit_state_e {
    TIME_LIMIT_NOINIT=0,
    TIME_LIMIT_RUNNING,
    TIME_LIMIT_DONE,
};
    
struct time_limit_t {
    enum time_limit_state_e state;
    pthread_t thread;
    size_t ms;
};

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

static void *
run_timer(void *args)
{
    struct time_limit_t *time = (struct time_limit_t *)args;

    size_t ms = 0;
    while (ms++ < time->ms) {
        usleep(1000);
    }
    time->state = TIME_LIMIT_DONE;
}

static int
time_limiter(IO_FILTER_ARGS)
{
    IOF_DISABLED();

    struct time_limit_t *time = (struct time_limit_t *)IO_FILTER_ARGS_FILTER->obj;
    switch (time->state) {
    case TIME_LIMIT_NOINIT:
        time->state = TIME_LIMIT_RUNNING;
        pthread_create(&time->thread, NULL, run_timer, (void *)time);
        break;

    case TIME_LIMIT_RUNNING:
        break;

    case TIME_LIMIT_DONE:
        pthread_join(time->thread, NULL);
        return IO_COMPLETE;

    default:
        return IO_ERROR;
    }
        
    return CALL_NEXT_FILTER();
}

struct io_filter_t *
create_time_limit_filter(void *alloc, const char *name, size_t time_ms)
{
    struct io_filter_t *f = create_filter(alloc, name, time_limiter);
    struct time_limit_t *time = pcalloc(alloc, sizeof(struct time_limit_t));
    time->ms = time_ms;
    f->obj = time;
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
