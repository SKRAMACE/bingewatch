#include <stdio.h>
#include <pthread.h>

#include "machine.h"
#include "segment.h"
#include "stream-state.h"

typedef void *(*pthread_fn)(void*);

#define SEGMENT_COMPLETE(s) s->complete.fn(s->complete.arg)
#define SEGMENT_ERROR(s) s->error.fn(s->error.arg)

#define DEFAULT_BUF_LEN 10*MB

// Externed in segment.h
__thread char seg_name[SEG_NAME_LEN] = {0};
static int segment_counter = 0;

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
    pthread_mutex_t *lock;          // Stream lock
    pthread_fn fn;                  // Function for running the segment

    // State machine
    enum stream_state_e *state;    // Pointer to stream state
    char running;                  // This controls the main loop.  TODO: This should be handled by the actual state
    char no_data;

    size_t bytes;

    // Seg Management
    int id;                     // Numeric value for identification
    POOL *pool;                 // Memory pool
    struct io_segment_t *next;  // Next segment in list

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

    s->running = 0;
}


static void
cleanup_segment(struct io_segment_t *seg)
{
    if (!seg) {
        return;
    }

    // Get IOM references
    const IOM *src = get_machine_ref(seg->in);
    const IOM *dst = get_machine_ref(seg->out);
    const IOM *dst1 = get_machine_ref(seg->out1);

    pthread_mutex_lock(seg->lock);

    // TODO: Print metrics from this segment

    pthread_mutex_unlock(seg->lock);
}


/* REFERENCE 
static void
calculate_metrics(struct benchmark_t *bm, size_t bytes)
{
    if (!bm) {
        return;
    }

    bm->total_bytes += bytes;
    bm->bytes += bytes;
    gettimeofday(&bm->t1, NULL);

    if (bm->bytes > METRIC_SAMPLE_THRESHOLD) {
        size_t t0_us = (1000000 * bm->t0.tv_sec) + bm->t0.tv_usec;
        size_t t1_us = (1000000 * bm->t1.tv_sec) + bm->t1.tv_usec;
        double elapsed_us = (t1_us - t0_us);
        bm->tp = (8 * bm->bytes)/elapsed_us;
        //printf("%f Mb/s\n", bm->tp);

        bm->bytes = 0;
        gettimeofday(&bm->t0, NULL);
    }
}
*/

static inline enum iostream_seg_ctrl_e
read_from_source(const IOM *src, struct io_segment_t *seg, char *buf, size_t *bytes)
{
    enum io_status status = src->read(seg->in, buf, bytes);
    switch (status) {
    case IO_SUCCESS:
        break;
    case IO_COMPLETE:
        printf("Segment %s read complete\n", src->name);
        SEGMENT_COMPLETE(seg);
        stop_segment(seg);
        break;
    default:
        printf("Segment %s read error\n", src->name);
        SEGMENT_ERROR(seg);
        stop_segment(seg);
        return SEG_CTRL_TOP;
    }

    if (0 == *bytes) {
        return SEG_CTRL_TOP;
    }

    // Calculate input metrics
    pthread_mutex_lock(seg->lock);
    //calculate_metrics(&seg->in_stats, *bytes);
    pthread_mutex_unlock(seg->lock);

    return SEG_CTRL_PROCEED;
}

static inline enum iostream_seg_ctrl_e
write_to_dest(const IOM *dst, int out_index, struct io_segment_t *seg, char *buf, size_t *bytes)
{
    // Set output index
    IO_HANDLE out;
    switch (out_index) {
    case 0:
        out = seg->out;
        break;
    case 1:
        out = seg->out1;
        break;
    default:
        printf("ERROR: Invalid segment output index (%d)\n", out_index);
        SEGMENT_ERROR(seg);
        stop_segment(seg);
        return SEG_CTRL_TOP;
    }

    // Write to dest 
    size_t remaining = *bytes;
    size_t wr_bytes = 0;
    char *ptr = buf;

    while (remaining) {
        size_t _bytes = remaining;
        enum io_status status = dst->write(out, ptr, &_bytes);
        switch (status) {
        case IO_SUCCESS:
            break;
        case IO_COMPLETE:
            printf("Segment %s write complete\n", dst->name);
            SEGMENT_COMPLETE(seg);
            stop_segment(seg);
            break;
        default:
            printf("Segment %s write error\n", dst->name);
            SEGMENT_ERROR(seg);
            stop_segment(seg);
            return SEG_CTRL_TOP;
        }

        if (_bytes > remaining) {
            printf("Segment %s counter error\n", dst->name);
            SEGMENT_ERROR(seg);
            stop_segment(seg);
            return SEG_CTRL_TOP;
        }

        remaining -= _bytes;
        ptr += _bytes;
        wr_bytes += _bytes;
    }

    // Calculate output metrics
    pthread_mutex_lock(seg->lock);
    //calculate_metrics(out_stats, wr_bytes);
    pthread_mutex_unlock(seg->lock);

    return SEG_CTRL_PROCEED;
}


void *
segment_run(void *arg)
{
    /* Arg management */
    struct io_segment_t *seg = (struct io_segment_t *)arg;
    const IOM *src = get_machine_ref(seg->in);
    const IOM *dst = get_machine_ref(seg->out);
    const IOM *dst1 = get_machine_ref(seg->out1);

    size_t buflen = (src->buf_read_size_rec > dst->buf_write_size_rec) ?
        src->buf_read_size_rec : dst->buf_write_size_rec;

    if (dst1) {
        buflen = (dst1->buf_write_size_rec > buflen) ? dst1->buf_write_size_rec : buflen;
    }

    if (0 == buflen) {
        buflen = DEFAULT_BUF_LEN;
    }

    /* Initialization */
    POOL *pool = create_pool();
    char *buf = palloc(pool, buflen);
    if (!buf) {
        printf("ERROR: Failed to allocate segment buffer.\n");
        SEGMENT_ERROR(seg);
    }

    pthread_mutex_lock(seg->lock);
    //gettimeofday(&seg->in_stats.t0, NULL);
    //gettimeofday(&seg->out_stats.t0, NULL);
    pthread_mutex_unlock(seg->lock);

    if (dst1) {
        snprintf(seg_name, SEG_NAME_LEN, "%s -> %s & %s (%d)",
            src->name, dst->name, dst1->name, (uint32_t)buflen);
    } else {
        snprintf(seg_name, SEG_NAME_LEN, "%s -> %s (%d)",
            src->name, dst->name, (uint32_t)buflen);
    }

    seg_name[SEG_NAME_LEN - 1] = '\0';
    printf("Starting Segment %s\n", seg_name);

    seg->running = 1;
    while (seg->running) {
        enum iostream_seg_ctrl_e ctrl;
        size_t bytes = 0;

        /* State Machine */
        switch (*seg->state) {
        case STREAM_READY:
            continue;
        case STREAM_RUNNING:
        case STREAM_FINISHING:
            break;
        case STREAM_DONE:
        case STREAM_INIT:
        case STREAM_ERROR:
        default:
            seg->running = 0;
            continue;
        }

        bytes = buflen;

        if (SEG_CTRL_TOP == read_from_source(src, seg, buf, &bytes)) {
            seg->no_data = 1;
            continue;
        }
        seg->no_data = 0;

        size_t src_bytes = bytes;
        if (SEG_CTRL_TOP == write_to_dest(dst, 0, seg, buf, &bytes)) {
            continue;
        }

        if (0 == seg->out1) {
            continue;
        }

        bytes = src_bytes;
        if (SEG_CTRL_TOP == write_to_dest(dst, 1, seg, buf, &bytes)) {
            continue;
        }
    }

    pfree(pool);
    cleanup_segment(seg);
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
    seg->pool = pool;
    seg->in = in;
    seg->out = out;
    seg->out1 = out1;
    seg->id = segment_counter++;
    seg->fn = segment_run;

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

void
segment_start(IO_SEGMENT seg, enum stream_state_e *state)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    //s->lock = &stream->lock;
    pthread_create(&s->thread, NULL, s->fn, (void *)s);
}

void
segment_join(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;
    pthread_join(s->thread, NULL);
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

    pthread_mutex_lock(s->lock);
    int ret = s->running;
    pthread_mutex_unlock(s->lock);

    return ret;
}

size_t
segment_bytes(IO_SEGMENT seg)
{
    struct io_segment_t *s = (struct io_segment_t *)seg;

    pthread_mutex_lock(s->lock);
    size_t ret = s->bytes;
    pthread_mutex_unlock(s->lock);

    return ret;
}