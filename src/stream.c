#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <linux/limits.h>
#include <sys/time.h>
#include <unistd.h>

#include "radpool.h"
#include "machine.h"
#include "stream.h"
#include "simple-buffers.h"

typedef void *(*pthread_fn)(void*);

#define DEFAULT_BUF_LEN 1*MB

/*** State Machine ***/
#define PRINT_STATE(s) (\
(s == SQ_INIT) ? "SQ_INIT" :(\
(s == SQ_READY) ? "SQ_READY" :(\
(s == SQ_RUNNING) ? "SQ_RUNNING" :(\
(s == SQ_FINISHING) ? "SQ_FINISHING" :(\
(s == SQ_DONE) ? "SQ_DONE" :(\
(s == SQ_ERROR) ? "SQ_ERROR" : "UNKNOWN STATE"))))))

enum sq_state_e {
    SQ_INIT,
    SQ_READY,
    SQ_RUNNING,
    SQ_FINISHING,
    SQ_DONE,
    SQ_ERROR,
};

enum iostream_seg_ctrl_e {
    SEG_CTRL_TOP,
    SEG_CTRL_PROCEED,
};

#define METRIC_SAMPLE_THRESHOLD (100*MB)
struct benchmark_t {
    uint64_t total_bytes;
    float bytes;
    struct timeval t0;
    struct timeval t1;
    float tp;
};

// Forward declaration so io_stream and io_segment can reference each other
struct io_segment_t;

// Unique identifier for streams
static IO_STREAM stream_counter = 0;
struct io_stream_t {
    // Thread variables
    pthread_t thread;         // Thread for stream state machine
    pthread_mutex_t lock;     // Stream lock (shared with segments)

    // State machine
    enum sq_state_e state;          // Stream state

    // Data
    IO_STREAM id;                   // Stream handle
    POOL *pool;                     // Memory pool
    struct io_segment_t *segments;  // Segments associated with this stream
    struct io_stream_t *next;       // Next stream in list
} *streams = NULL;

// Unique identifier for streams
static IO_SEGMENT segment_counter = 0;
struct io_segment_t {
    struct io_stream_t *stream; // Segment's stream

    // Thread variables
    pthread_t thread;               // Thread for this segment
    pthread_mutex_t *lock;          // Stream lock
    pthread_fn fn;                  // Function for running the segment

    // State machine
    const enum sq_state_e *const state;  // Pointer to stream state
    char running;                        // This controls the main loop.  TODO: This should be handled by the actual state

    // Seg Management
    IO_SEGMENT id;              // Handle for this segment
    POOL *pool;                 // Memory pool
    struct io_segment_t *next;  // Next segment in list

    // Input
    IO_HANDLE in;                 // Input IOM
    struct benchmark_t in_stats;  // Input benchmark metrics

    // Output
    IO_HANDLE out;                 // Output IOM
    struct benchmark_t out_stats;  // Output benchmark metrics
    IO_HANDLE out1;                // Output IOM
    struct benchmark_t out1_stats; // Output benchmark metrics
};

// Externed in stream.h
__thread char stream_name[STREAM_NAME_LEN] = {0};

static void
set_state(struct io_stream_t *stream, enum sq_state_e new_state)
{
    if (new_state > SQ_ERROR) {
        printf("Invalid State (%d)\n", stream->state);
        new_state = SQ_ERROR;
    }

    printf("%s:%d - State change from %s to %s\n",
            __FUNCTION__, __LINE__, PRINT_STATE(stream->state), PRINT_STATE(new_state));
    stream->state = new_state;
}

static void
start_segments(struct io_stream_t *stream)
{
    struct io_segment_t *s = stream->segments;
    while (s) {
        // Segment references the stream state
        //      This convoluted assignment is to overcome the "const" qualifiers
        enum sq_state_e **seg_state = (enum sq_state_e **)&s->state;
        *seg_state = &stream->state;
        s->lock = &stream->lock;
        pthread_create(&s->thread, NULL, s->fn, (void *)s);
        printf("pthread %d started\n", (int)s->thread);
        s = s->next;
    }
}

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
join_stream_segments(struct io_stream_t *stream)
{
    struct io_segment_t *seg = stream->segments;
    while (seg) {
        pthread_join(seg->thread, NULL);
        seg = seg->next;
    }
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

    // Print metrics from this segment
    printf("metric %s -> %s:\n", src->name, dst->name);
    if (dst1) {
        printf("          -> %s:\n", dst1->name);
    }
    printf("metric %s -> %s:\n", src->name, dst->name);
    printf("  src: %" PRIu64 "\n", seg->in_stats.total_bytes);
    printf("  dst: %" PRIu64 "\n\n", seg->out_stats.total_bytes);
    if (dst1) {
        printf("  dst: %" PRIu64 "\n\n", seg->out1_stats.total_bytes);
    }

    pthread_mutex_unlock(seg->lock);
}

/*
 * Advance state machine on completion
 */
static void
callback_complete(struct io_stream_t *stream)
{
    pthread_mutex_lock(&stream->lock);
    switch (stream->state) {
    case SQ_INIT:
    case SQ_READY:
        set_state(stream, SQ_DONE);
        break;

    case SQ_RUNNING:
        set_state(stream, SQ_FINISHING);
        break;

    default:
        break;
    }

    pthread_mutex_unlock(&stream->lock);
}
/*
 * Advance state machine on completion
 */
static void
callback_error(struct io_stream_t *stream)
{
    pthread_mutex_lock(&stream->lock);
    switch (stream->state) {
    case SQ_ERROR:
    case SQ_FINISHING:
    case SQ_DONE:
        break;

    default:
        set_state(stream, SQ_ERROR);
        break;
    }

    pthread_mutex_unlock(&stream->lock);
}

static void *
main_state_machine(void *args)
{
    struct io_stream_t *st = (struct io_stream_t *)args;

    set_state(st, SQ_READY);

    start_segments(st);
    set_state(st, SQ_RUNNING);

    // Wait for
    //  1) A segment to signal completion
    //  2) A segment to signal error
    //  3) A shutdown from the main thread
    while (SQ_RUNNING == st->state) {
        usleep(500000);
    }

    // Wait for segments to complete final transactions
    if (SQ_FINISHING == st->state) {
        // TODO: Query the segments until they are all empty
        sleep(1);
    }

    set_state(st, SQ_DONE);

    join_stream_segments(st);

    pthread_exit(NULL);
}

static void
add_stream(struct io_stream_t *stream)
{
    struct io_stream_t *s = streams;
    if (!s) {
        streams = stream;
    } else {
        while (s->next) {
            s = s->next;
        }
        s->next = stream;
    }
}

static struct io_stream_t *
get_stream(IO_STREAM h)
{
    struct io_stream_t *s = streams;
    while (s) {
        if (s->id == h) {
            return s;
        }
        s = s->next;
    }
    return NULL;
}

static struct io_segment_t *
get_segment(IO_SEGMENT h)
{
    struct io_stream_t *st = streams;
    while (st) {
        struct io_segment_t *seg = st->segments;
        while (seg) {
            if (seg->id == h) {
                return seg;
            }
            seg = seg->next;
        }
        st = st->next;
    }

    printf("Error: Segment %d not found\n", h);
    return NULL;
}

static void
calculate_metrics(struct benchmark_t *bm, uint64_t bytes)
{
    if (!bm) {
        return;
    }

    bm->total_bytes += bytes;
    bm->bytes += bytes;
    gettimeofday(&bm->t1, NULL);

    if (bm->bytes > METRIC_SAMPLE_THRESHOLD) {
        uint64_t t0_us = (1000000 * bm->t0.tv_sec) + bm->t0.tv_usec;
        uint64_t t1_us = (1000000 * bm->t1.tv_sec) + bm->t1.tv_usec;
        double elapsed_us = (t1_us - t0_us);
        bm->tp = (8 * bm->bytes)/elapsed_us;
        //printf("%f Mb/s\n", bm->tp);

        bm->bytes = 0;
        gettimeofday(&bm->t0, NULL);
    }
}

static inline enum iostream_seg_ctrl_e
read_from_source(const IOM *src, struct io_segment_t *seg, char *buf, uint64_t *bytes)
{
    enum io_status status = src->read(seg->in, buf, bytes);
    switch (status) {
    case IO_SUCCESS:
        break;
    case IO_COMPLETE:
        printf("Segment %s read complete\n", src->name);
        callback_complete(seg->stream);
        stop_segment(seg);
        break;
    default:
        printf("Segment %s read error\n", src->name);
        callback_error(seg->stream);
        stop_segment(seg);
        return SEG_CTRL_TOP;
    }

    if (0 == bytes) {
        return SEG_CTRL_TOP;
    }

    // Calculate input metrics
    pthread_mutex_lock(seg->lock);
    calculate_metrics(&seg->in_stats, *bytes);
    pthread_mutex_unlock(seg->lock);

    return SEG_CTRL_PROCEED;
}

static inline enum iostream_seg_ctrl_e
write_to_dest(const IOM *dst, int out_index, struct io_segment_t *seg, char *buf, uint64_t *bytes)
{
    // Set output index
    IO_HANDLE out;
    struct benchmark_t *out_stats;
    switch (out_index) {
    case 0:
        out = seg->out;
        out_stats = &seg->out_stats;
        break;
    case 1:
        out = seg->out1;
        out_stats = &seg->out1_stats;
        break;
    default:
        printf("ERROR: Invalid segment output index (%d)\n", out_index);
        callback_error(seg->stream);
        stop_segment(seg);
        return SEG_CTRL_TOP;
    }

    // Write to dest 
    uint64_t remaining = *bytes;
    uint64_t wr_bytes = 0;
    char *ptr = buf;

    while (remaining) {
        enum io_status status = dst->write(out, ptr, bytes);
        switch (status) {
        case IO_SUCCESS:
            break;
        case IO_COMPLETE:
            printf("Segment %s write complete\n", dst->name);
            callback_complete(seg->stream);
            stop_segment(seg);
            break;
        default:
            printf("Segment %s write error\n", dst->name);
            callback_error(seg->stream);
            stop_segment(seg);
            return SEG_CTRL_TOP;
        }

        remaining -= *bytes;
        ptr += *bytes;
        wr_bytes += *bytes;
    }

    // Calculate output metrics
    pthread_mutex_lock(seg->lock);
    calculate_metrics(out_stats, wr_bytes);
    pthread_mutex_unlock(seg->lock);

    return SEG_CTRL_PROCEED;
}

/*
 * A stream segment is a pthread which connects two bingewatch
 */
static void *
generic_stream_segment(void *arg)
{
    /* Arg management */
    struct io_segment_t *seg = (struct io_segment_t *)arg;
    const IOM *src = get_machine_ref(seg->in);
    const IOM *dst = get_machine_ref(seg->out);
    const IOM *dst1 = get_machine_ref(seg->out1);

    uint64_t buflen = (src->buf_size_rec > dst->buf_size_rec) ? src->buf_size_rec : dst->buf_size_rec;
    if (dst1) {
        buflen = (dst1->buf_size_rec > buflen) ? dst1->buf_size_rec : buflen;
    }

    if (0 == buflen) {
        buflen = DEFAULT_BUF_LEN;
    }

    /* Initialization */
    POOL *pool = create_pool();
    char *buf = pcalloc(pool, buflen);
    if (!buf) {
        fprintf(stderr, "ERROR: Failed to allocate segment buffer.\n");
        set_state(seg->stream, SQ_ERROR);
    }

    pthread_mutex_lock(seg->lock);
    gettimeofday(&seg->in_stats.t0, NULL);
    gettimeofday(&seg->out_stats.t0, NULL);
    pthread_mutex_unlock(seg->lock);

    snprintf(stream_name, STREAM_NAME_LEN, "%s_%s", src->name, dst->name);
    stream_name[STREAM_NAME_LEN - 1] = '\0';
    printf("Starting Segment %s\n", stream_name);

    seg->running = 1;
    while (seg->running) {
        enum iostream_seg_ctrl_e ctrl;
        uint64_t bytes = 0;

        /* State Machine */
        switch (*seg->state) {
        case SQ_READY:
            continue;
        case SQ_RUNNING:
        case SQ_FINISHING:
            break;
        case SQ_DONE:
        case SQ_INIT:
        case SQ_ERROR:
        default:
            seg->running = 0;
            continue;
        }

        bytes = buflen;

        if (SEG_CTRL_TOP == read_from_source(src, seg, buf, &bytes)) {
            continue;
        }

        uint64_t src_bytes = bytes;
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

/*
 * Create a new stream struct and add it to the stream list.  Return the stream handle.
 */
IO_STREAM
new_stream()
{
    POOL *p = create_pool();
    struct io_stream_t *stream = pcalloc(p, sizeof(struct io_stream_t));
    stream->state = SQ_INIT;
    stream->pool = p;
    stream->segments = NULL;
    pthread_mutex_init(&stream->lock, NULL);
    stream->id = ++stream_counter;

    add_stream(stream);
    return stream->id;
}

/*
 * Create a new segment struct and add it to the stream.
 */
IO_SEGMENT
io_stream_add_segment_internal(struct io_stream_t *st, IO_HANDLE in, IO_HANDLE out, IO_HANDLE out1)
{
    // Create a pool for the segment
    POOL *pool = create_subpool(st->pool);
    struct io_segment_t *seg = pcalloc(pool, sizeof(struct io_segment_t));

    // Initialize segment
    seg->stream = st;
    seg->pool = pool;
    seg->in = in;
    seg->out = out;
    seg->out1 = out1;
    seg->id = ++segment_counter;
    seg->fn = generic_stream_segment;

    // Add segment to the stream
    struct io_segment_t *s = st->segments;
    if (!s) {
        st->segments = seg;
    } else {
        while (s->next) {
            s = s->next;
        }
        s->next = seg;
    }
    return seg->id;
}

IO_SEGMENT
io_stream_add_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return 0;
    }

    // Init Asynchronous Buffer IO Machine
    const IOM *rb = get_rb_machine();
    struct rbiom_args rb_vars = {0, 0, 0};
    IO_HANDLE buf = rb->create(&rb_vars);

    io_stream_add_segment_internal(st, buf, out, 0);
    io_stream_add_segment_internal(st, in, buf, 0);
}

IO_SEGMENT
io_stream_add_tee_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out0, IO_HANDLE out1)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return 0;
    }

    // Init Asynchronous Buffer IO Machine
    const IOM *rb = get_rb_machine();
    struct rbiom_args rb_vars = {0, 0, 0};
    IO_HANDLE buf0 = rb->create(&rb_vars);
    IO_HANDLE buf1 = rb->create(&rb_vars);

    io_stream_add_segment_internal(st, buf0, out0, 0);
    io_stream_add_segment_internal(st, buf1, out1, 0);
    io_stream_add_segment_internal(st, in, buf0, buf1);
}

void
segment_add_custom_function(IO_SEGMENT h, pthread_fn fn)
{
    struct io_segment_t *seg = get_segment(h);
    if (!seg) {
        printf("Error: Failed to add custom function to segment\n");
        return;
    }
    seg->fn = fn;
}

int
start_stream(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return IO_ERROR;
    }

    // Run the stream as its own thread
    pthread_create(&st->thread, NULL, main_state_machine, (void *)st);
    return IO_SUCCESS;
}

void
join_stream(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return;
    }

    pthread_join(st->thread, NULL);
}

static void
destroy_machine(IO_HANDLE h)
{
    if (h == 0) {
        return;
    }

    const IOM *machine = get_machine_ref(h);
    if (machine) {
        printf("\t\t\tIOM %d: Destroying\n", h);
        machine->destroy(h);
    }
}

static void
destroy_stream(struct io_stream_t *st)
{
    struct io_segment_t *seg = st->segments;
    while (seg) {
        printf("\t\tSeg %d: Shutting down\n", seg->id);
        destroy_machine(seg->in);
        destroy_machine(seg->out);
        destroy_machine(seg->out1);
        seg = seg->next;
    }
}

/*
 * Stop all streams, handling full state machine
 */
static void
stop_stream_internal(struct io_stream_t *st)
{
    int done = 0;
    while (!done) {
        switch (st->state) {
        // wait for stream to start running
        case SQ_INIT:
        case SQ_READY:
            usleep(1000000);
            continue;

        // send completion signal to the stream
        case SQ_RUNNING:
            printf("\tStream %d: Shutting down\n", st->id);
            callback_complete(st);
            break;

        // stream is already set to stop
        case SQ_FINISHING:
        case SQ_DONE:
            break;
        case SQ_ERROR:
            printf("ERROR\n");
            break;
        default:
            break;
        }
        done = 1;
    }
}

/*
 * Stop a single stream from its handle
 */
int
stop_stream(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return IO_ERROR;
    }

    stop_stream_internal(st);
    return IO_SUCCESS;
}

/*
 * Signal all streams to begin the completion process
 */
void
stop_streams()
{
    struct io_stream_t *st = streams;
    while (st) {
        printf("\tStopping stream %d\n", st->id);
        stop_stream_internal(st);
        st = st->next;
    }
}

void
stream_cleanup()
{
    struct io_stream_t *st = streams;
    while (st) {
        stop_stream_internal(st);
        pthread_join(st->thread, NULL);
        destroy_stream(st);
        st = st->next;
    }
}
