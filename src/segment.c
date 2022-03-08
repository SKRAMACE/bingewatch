#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "machine.h"
#include "segment.h"
#include "ring-buf.h"
#include "bw-util.h"
#include "stream-state.h"

#define LOGEX_TAG "BW-SEG"
#include "bw-log.h"

typedef void *(*pthread_fn)(void*);

static char *gsep = "-";
static char *group = "";

static int metrics_enabled = 0;
#define seg_metrics(s, x, ...) \
 if (metrics_enabled == 1) { \
     _logex_log_print(INFO, "MET", "%s%s%s: " x, *s->group, s->gsep, s->name, ##__VA_ARGS__); \
 }

#define seg_error(s, x, ...) error("%s%s%s: " x, *s->group, s->gsep, s->name, ##__VA_ARGS__)
#define seg_info(s, x, ...) info("%s%s%s: " x, *s->group, s->gsep, s->name, ##__VA_ARGS__)
#define seg_trace(s, x, ...) trace("%s%s%s: " x, *s->group, s->gsep, s->name, ##__VA_ARGS__)
#define seg_debug(s, x, ...) debug("%s%s%s: " x, *s->group, s->gsep, s->name, ##__VA_ARGS__)

#define SEGMENT_COMPLETE(s) s->complete.fn(s->complete.arg)

#define SEGMENT_ERROR(s) s->error.fn(s->error.arg)

#define SEG_GRP(x) ((x->group) ? *x->group"-" : "")
#define SEGMENT_NAME_LEN 1024
#define SEGMENT_DEFAULT_BUFLEN 10*MB

static int segment_counter = 0;

enum segment_direction_e {
    SEG_DIR_IN,
    SEG_DIR_OUT,
    SEG_DIR_OUT1,
};

enum iostream_seg_ctrl_e {
    SEG_CTRL_TOP,
    SEG_CTRL_PROCEED,
};

struct seg_callback_t {
    seg_callback fn;
    void *arg;
};

struct io_segment_t {
    struct seg_callback_t error;
    struct seg_callback_t complete;

    // Thread variables
    pthread_t thread;               // Thread for this segment
    pthread_mutex_t lock;           // Segment lock
    pthread_fn fn;                  // Function for running the segment

    // State machine
    enum stream_state_e *state;    // Pointer to stream state
    char running;                  // This controls the main loop
    char do_complete;              // This controls the main loop

    // Seg Management
    int id;                     // Numeric value for identification
    char **group;               // Segment group (usually pointer to stream name)
    char *gsep;                // Separator between group and name
    char name[SEGMENT_NAME_LEN];// Segment name
    POOL *pool;                 // Memory pool
    struct io_segment_t *next;  // Next segment in list
    size_t default_buf_len;

    // Input
    IO_HANDLE in;                 // Input IOM

    // Output
    IO_HANDLE out;                 // Output IOM
    IO_HANDLE out1;                // Output IOM
};

/*
 * Stop IO Machines and set running flag to false
 */
static void
stop_segment(struct io_segment_t *s)
{
    const IOM *src = get_machine_ref(s->in);
    const IOM *dst = get_machine_ref(s->out);
    const IOM *dst1 = get_machine_ref(s->out1);

    if (src) {
        src->stop(s->in);
    }
    
    if (dst) {
        dst->stop(s->out);
    }

    if (dst1) {
        dst1->stop(s->out1);
    }

    seg_trace(s, "Stop command issued");
    s->running = 0;
    s->do_complete = 0;
}

static inline void
read_from_source(struct io_segment_t *seg, IO_DESC *src, char *buf, size_t *bytes)
{
    IO_HANDLE h = src->handle;
    enum io_status status = src->machine->read(h, buf, bytes);

    if (IO_COMPLETE == status) {
        seg_info(seg, "Read complete");
        seg->do_complete = 1;

    } else if (status < IO_SUCCESS) {
        seg_error(seg, "Read error (%d)", status);
        SEGMENT_ERROR(seg);
        stop_segment(seg);
        *bytes = 0;
    }
}

static void
write_to_dest(struct io_segment_t *seg, IO_DESC *dst, char *buf, size_t *bytes)
{
    // Set output index
    IO_HANDLE h = dst->handle;

    // Write to dest 
    size_t remaining = *bytes;
    size_t wr_bytes = 0;
    char *ptr = buf;

    while (remaining) {
        size_t _bytes = remaining;
        enum io_status status = dst->machine->write(h, ptr, &_bytes);
        if (IO_COMPLETE == status) {
            seg_info(seg, "Write complete");
            seg->do_complete = 1;
            break;

        } else if (status < IO_SUCCESS) {
            seg_error(seg, "Write error (%d)", status);
            SEGMENT_ERROR(seg);
            stop_segment(seg);
            wr_bytes = 0;
            break;
        }

        remaining -= _bytes;
        ptr += _bytes;
        wr_bytes += _bytes;
    }

    *bytes = wr_bytes;
}

static void *
segment_run_source(void *arg)
{
    /* Arg management */
    struct io_segment_t *seg = (struct io_segment_t *)arg;
    IO_DESC *src = machine_get_desc(seg->in);
    IO_DESC *rb = machine_get_desc(seg->out);

    size_t buflen = (src->io_read->size > rb->io_write->size) ?
        src->io_read->size : rb->io_write->size;

    //if (0 == buflen) {
    //    buflen = seg->default_buf_len;
    //}

    seg_trace(seg, "Starting segment source");

    seg->running = 1;
    while (seg->running) {
        enum stream_state_e state = *seg->state;
        if (STREAM_READY == state) {
            continue;
        }

        if (seg->do_complete) {
            SEGMENT_COMPLETE(seg);
            stop_segment(seg);
            continue;
        }

        if (!STREAM_IS_RUNNING(state)) {
            seg_trace(seg, "Stream stopped");
            seg->running = 0;
            continue;
        }

        const struct __block_t *b;
        if (rb_acquire_write_block(seg->out, buflen, &b) < 0) {
            seg_error(seg, "Write error (%d)", IO_ERROR);
            SEGMENT_ERROR(seg);
            stop_segment(seg);
            goto no_data;
        }

        if (!b) {
            goto no_data;
        }

        size_t bytes = b->size;
        read_from_source(seg, src, b->data, &bytes);
        rb_release_write_block(seg->out, bytes);

        if (bytes == 0) {
            goto no_data;
        }
        continue;

    no_data:
        usleep(1000);
    }

    pthread_exit(NULL);
}

static void *
segment_run(void *arg)
{
    /* Arg management */
    struct io_segment_t *seg = (struct io_segment_t *)arg;
    IO_DESC *src = machine_get_desc(seg->in);
    IO_DESC *dst = machine_get_desc(seg->out);
    IO_DESC *dst1 = machine_get_desc(seg->out1);

    size_t buflen = (src->io_read->size > dst->io_write->size) ?
        src->io_read->size : dst->io_write->size;

    if (dst1) {
        buflen = (dst1->io_write->size > buflen) ? dst1->io_write->size : buflen;
    }

    if (0 == buflen) {
        buflen = seg->default_buf_len;
    }

    /* Initialization */
    POOL *pool = create_pool();
    char *buf = palloc(pool, buflen);
    if (!buf) {
        seg_error(seg, "Failed to allocate buffer");
        SEGMENT_ERROR(seg);
    }

    //pthread_mutex_lock(&seg->lock);
    //gettimeofday(&seg->in_stats.t0, NULL);
    //gettimeofday(&seg->out_stats.t0, NULL);
    //pthread_mutex_unlock(&seg->lock);

    seg_trace(seg, "Starting segment");

    seg->running = 1;
    while (seg->running) {
        enum stream_state_e state = *seg->state;
        if (STREAM_READY == state) {
            continue;
        }

        if (seg->do_complete) {
            SEGMENT_COMPLETE(seg);
            stop_segment(seg);
            continue;
        }

        if (!STREAM_IS_RUNNING(state)) {
            seg_trace(seg, "Stream stopped");
            seg->running = 0;
            continue;
        }

        size_t bytes = buflen;
        read_from_source(seg, src, buf, &bytes);

        if (bytes == 0) {
            usleep(1000);
            /*
            if (*seg->state == STREAM_FINISHING) {
                seg_trace(seg, "Source empty");
                seg->running = 0;
            }
            */
            continue;
        }

        size_t src_bytes = bytes;
        write_to_dest(seg, dst, buf, &bytes);
        if (bytes == 0) {
            continue;
        } else if (bytes != src_bytes) {
            error("Partial write");
        }

        if (seg->out1 > 0) {
            bytes = src_bytes;
            write_to_dest(seg, dst1, buf, &bytes);
        }
    }

    free_pool(pool);
    pthread_exit(NULL);
}

void
segment_register_callback_complete(void *segment, seg_callback fn, void *arg)
{
    struct io_segment_t *s = (struct io_segment_t *)segment;
    s->complete.fn = fn;
    s->complete.arg = arg;
}

void
segment_register_callback_error(void *segment, seg_callback fn, void *arg)
{
    struct io_segment_t *s = (struct io_segment_t *)segment;
    s->error.fn = fn;
    s->error.arg = arg;
}

static IO_SEGMENT
segment_create(POOL *pool, IO_HANDLE in, IO_HANDLE out, IO_HANDLE out1)
{
    // Create a pool for the segment
    POOL *p = create_subpool(pool);
    struct io_segment_t *seg = pcalloc(pool, sizeof(struct io_segment_t));

    // Initialize segment
    pthread_mutex_init(&seg->lock, NULL);

    pthread_mutex_lock(&seg->lock);
    seg->pool = pool;
    seg->in = in;
    seg->out = out;
    seg->out1 = out1;
    seg->id = segment_counter++;
    seg->group = &group;
    seg->gsep = "";
    seg->default_buf_len = SEGMENT_DEFAULT_BUFLEN;
    seg->fn = segment_run;

    snprintf(seg->name, SEGMENT_NAME_LEN-1, "seg%d", seg->id);

    pthread_mutex_unlock(&seg->lock);

    return (IO_SEGMENT)seg;
}

IO_SEGMENT
segment_create_1_1(POOL *pool, IO_HANDLE in, IO_HANDLE out)
{
    return segment_create(pool, in, out, 0);
}

IO_SEGMENT
segment_create_1_2(POOL *pool, IO_HANDLE in, IO_HANDLE out0, IO_HANDLE out1)
{
    return segment_create(pool, in, out0, out1);
}

IO_SEGMENT
segment_create_src(POOL *pool, IO_HANDLE src, IO_HANDLE *buf)
{
    IO_HANDLE src_buf = new_rb_machine();

    IO_SEGMENT seg = segment_create(pool, src, src_buf, 0);
    struct io_segment_t *s = (struct io_segment_t *)seg;
    s->fn = segment_run_source;

    *buf = src_buf;
    return seg;
}

void
segment_start(IO_SEGMENT seg, enum stream_state_e *state)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    s->state = state;
    pthread_create(&s->thread, NULL, s->fn, (void *)s);
}

void
segment_join(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    pthread_join(s->thread, NULL);
}

static void
segment_print_metrics_internal(struct io_segment_t *seg, enum segment_direction_e dir)
{
    IO_HANDLE h = 0;
    switch (dir) {
    case SEG_DIR_IN:
        h = seg->in;
        break;

    case SEG_DIR_OUT:
        h = seg->out;
        break;

    case SEG_DIR_OUT1:
        h = seg->out1;
        break;
    }

    if (h == 0) {
        return;
    }

    struct io_metrics_t *m = (struct io_metrics_t *)machine_metrics(h);
    if (!m) {
        return;
    }

    // A segment's input is a machine's output, and visa versa
    char mstr[1024];
    switch (dir) {
    case SEG_DIR_IN:
        machine_metrics_fmt(&m->out, mstr, 1024,
            METRICS_FMT_TYPE_ONELINE | METRICS_CALC_TYPE_FULL);
        seg_metrics(seg, "I: %s", mstr);
        break;
    case SEG_DIR_OUT:
    case SEG_DIR_OUT1:
        machine_metrics_fmt(&m->in, mstr, 1024,
            METRICS_FMT_TYPE_ONELINE | METRICS_CALC_TYPE_FULL);
        seg_metrics(seg, "O: %s", mstr);
        break;
    default:
        break;
    }
}

void
segment_print_metrics(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;

    if (s->in == 0 && s->out == 0 && s->out1 == 0) {
        return;
    }

    segment_print_metrics_internal(s, SEG_DIR_IN);
    segment_print_metrics_internal(s, SEG_DIR_OUT);
    segment_print_metrics_internal(s, SEG_DIR_OUT1);
}

static void
destroy_machine(IO_HANDLE h)
{
    if (h == 0) {
        return;
    }

    const IOM *machine = get_machine_ref(h);
    if (machine) {
        machine->destroy(h);
    }
}

void
segment_destroy(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    destroy_machine(s->in);
    destroy_machine(s->out);
    destroy_machine(s->out1);
}

int
segment_is_running(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;

    pthread_mutex_lock(&s->lock);
    int ret = s->running;
    pthread_mutex_unlock(&s->lock);

    return ret;
}

void
segment_set_group(IO_SEGMENT seg, char **group)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    s->group = group;
    s->gsep = gsep;
}


void
segment_set_name(IO_SEGMENT seg, char *name)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    snprintf(s->name, SEGMENT_NAME_LEN-1, "%s", name);
}

void
segment_set_default_buflen(IO_SEGMENT seg, size_t len)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;

    pthread_mutex_lock(&s->lock);
    s->default_buf_len = len;
    pthread_mutex_unlock(&s->lock);
}

void
segment_enable_metrics(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;

    machine_metrics_enable(s->in);
    machine_metrics_enable(s->out);
    machine_metrics_enable(s->out1);

    metrics_enabled = 1;
}

void
segment_set_log_level(char *level)
{
    bw_set_log_level_str(level);
}
