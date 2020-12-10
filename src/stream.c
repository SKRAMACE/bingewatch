#include <pthread.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/time.h>
#include <unistd.h>

#include "machine.h"
#include "stream.h"
#include "stream-state.h"
#include "segment.h"

#include "simple-buffers.h"

static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;

// Unique identifier for streams
static IO_STREAM stream_counter = 0;
struct io_stream_t {
    // Thread variables
    pthread_t thread;           // Thread for stream state machine
    pthread_mutex_t lock;       // Stream lock (shared with segments)

    // State machine
    enum stream_state_e state;  // Stream state

    // Data
    IO_STREAM id;               // Stream handle
    POOL *pool;                 // Memory pool

    // Segments
    IO_SEGMENT *segments;       // Segments associated with this stream
    int n_segment;              // Number of segments
    int segment_len;            // Segment entries

    struct io_stream_t *next;   // Next stream in list
} *streams = NULL;

static void
set_state(struct io_stream_t *stream, enum stream_state_e new_state)
{
    if (new_state > STREAM_ERROR) {
        printf("Invalid State (%d)\n", stream->state);
        new_state = STREAM_ERROR;
    }

    printf("%s:%d - State change from %s to %s\n", __FUNCTION__, __LINE__,
        STREAM_STATE_PRINT(stream->state), STREAM_STATE_PRINT(new_state));

    stream->state = new_state;
}

static void
start_segments(struct io_stream_t *stream)
{
    int s = 0;
    for (; s < stream->n_segment; s++) {
        IO_SEGMENT seg = stream->segments[s];
        segment_start(seg, &stream->state);
    }
}

static void
join_stream_segments(struct io_stream_t *stream)
{
    int s = 0;
    for (; s < stream->n_segment; s++) {
        IO_SEGMENT seg = stream->segments[s];
        segment_join(seg);
    }
}

static void
callback_complete(void *arg)
{
    struct io_stream_t *stream = (struct io_stream_t *)arg;
    pthread_mutex_lock(&stream->lock);
    switch (stream->state) {
    case STREAM_INIT:
    case STREAM_READY:
        set_state(stream, STREAM_DONE);
        break;

    case STREAM_RUNNING:
        set_state(stream, STREAM_FINISHING);
        break;

    default:
        break;
    }

    pthread_mutex_unlock(&stream->lock);
}

static void
callback_error(void *arg)
{
    struct io_stream_t *stream = (struct io_stream_t *)arg;
    pthread_mutex_lock(&stream->lock);
    switch (stream->state) {
    case STREAM_ERROR:
    case STREAM_FINISHING:
    case STREAM_DONE:
        break;

    default:
        set_state(stream, STREAM_ERROR);
        break;
    }

    pthread_mutex_unlock(&stream->lock);
}

static void
flush_stream_segments(struct io_stream_t *st)
{
    int s = 0;
    while (s < st->n_segment) {
        IO_SEGMENT seg = st->segments[s];

        if (!segment_is_running(seg)) {
            goto next;
        }
            
        if (segment_bytes(seg) == 0) {
            goto next;
        }
    next:
        s++;
    }
}

static void *
main_state_machine(void *args)
{
    struct io_stream_t *st = (struct io_stream_t *)args;

    set_state(st, STREAM_READY);

    start_segments(st);
    set_state(st, STREAM_RUNNING);

    // Wait for
    //  1) A segment to signal completion
    //  2) A segment to signal error
    //  3) A shutdown from the main thread
    while (STREAM_RUNNING == st->state) {
        usleep(500000);
    }

    // Wait for segments to complete final transactions
    if (STREAM_FINISHING == st->state) {
        flush_stream_segments(st);
    }

    set_state(st, STREAM_DONE);

    join_stream_segments(st);

    pthread_exit(NULL);
}

static void
add_stream(struct io_stream_t *stream)
{
    pthread_mutex_lock(&stream_lock);
    struct io_stream_t *s = streams;
    if (!s) {
        streams = stream;
    } else {
        while (s->next) {
            s = s->next;
        }
        s->next = stream;
    }
    pthread_mutex_unlock(&stream_lock);
}

static struct io_stream_t *
get_stream(IO_STREAM h)
{
    pthread_mutex_lock(&stream_lock);
    struct io_stream_t *s = streams;
    while (s) {
        if (s->id == h) {
            return s;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&stream_lock);

    return NULL;
}

/*
 * Create a new stream struct and add it to the stream list.  Return the stream handle.
 */
IO_STREAM
new_stream()
{
    POOL *p = create_pool();
    struct io_stream_t *stream = pcalloc(p, sizeof(struct io_stream_t));
    stream->state = STREAM_INIT;
    stream->pool = p;
    stream->segments = NULL;
    pthread_mutex_init(&stream->lock, NULL);
    stream->id = ++stream_counter;

    add_stream(stream);
    return stream->id;
}

static void
add_segment(struct io_stream_t *st, IO_SEGMENT seg)
{
    pthread_mutex_lock(&st->lock);

    int i = st->n_segment++;
    if (i >= st->segment_len) {
        st->segment_len += 10;
        st->segments = repalloc(st->segments, sizeof(int) * st->segment_len, st->pool);
    }

    st->segments[i] = seg;
    pthread_mutex_unlock(&st->lock);
}

int
io_stream_add_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out, int flag)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return 1;
    }

    IO_SEGMENT s;
    if (flag & BW_BUFFERED) {
        // Init Asynchronous Buffer IO Machine
        const IOM *rb = get_rb_machine();
        struct rbiom_args rb_vars = {0, 0, 0};
        IO_HANDLE buf = rb->create(&rb_vars);

        s = segment_create_1_1(st->pool, in, buf);
        add_segment(st, s);

        s = segment_create_1_1(st->pool, buf, out);
        add_segment(st, s);

    } else {
        s = segment_create_1_1(st->pool, in, out);
        add_segment(st, s);
    }

    return 0;
}

int
io_stream_add_tee_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out, IO_HANDLE out1, int flag)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        printf("ERROR: Stream not found\n");
        return 1;
    }

    if (flag & BW_BUFFERED) {
        // Init Asynchronous Buffer IO Machine
        const IOM *rb = get_rb_machine();
        struct rbiom_args rb_vars = {0, 0, 0};
        IO_HANDLE buf0 = rb->create(&rb_vars);
        IO_HANDLE buf1 = rb->create(&rb_vars);

        segment_create_1_2(st->pool, in, buf0, buf1);
        segment_create_1_1(st->pool, buf0, out);
        segment_create_1_1(st->pool, buf1, out1);
    } else {
        segment_create_1_2(st->pool, in, out, out1);
    }

    return 0;
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
stop_stream_internal(struct io_stream_t *st)
{
    while (1) {
        switch (st->state) {
        // wait for stream to start running
        case STREAM_INIT:
        case STREAM_READY:
            // TODO: Use an init lock
            // printf("\tStream %d: Still initializing\n", st->id);
            usleep(1000);
            continue;

        // send completion signal to the stream
        case STREAM_RUNNING:
            printf("\tStream %d: Shutting down\n", st->id);
            callback_complete(st);
            return;

        // stream is already set to stop
        case STREAM_FINISHING:
            printf("\tStream %d: Waiting for data to empty\n", st->id);
            return;

        case STREAM_DONE:
            return;

        case STREAM_ERROR:
            printf("\tStream %d: Error\n", st->id);
            return;

        default:
            return;
        }
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
    pthread_mutex_lock(&stream_lock);
    struct io_stream_t *st = streams;
    while (st) {
        stop_stream_internal(st);
        st = st->next;
    }
    pthread_mutex_unlock(&stream_lock);
}

/*
 * Free stream memory
 */
void
stream_cleanup()
{
    stop_streams();

    pthread_mutex_lock(&stream_lock);

    // Wait for streams to complete
    struct io_stream_t *st = streams;
    while (st) {
        pthread_join(st->thread, NULL);
    }

    while (st) {
        int s = 0;
        for (; s < st->n_segment; s++) {
            IO_SEGMENT seg = st->segments[s];
            segment_destroy(seg);
        }
        
        void *destroy_me = st;
        st = st->next;
        pfree(destroy_me);
    }

    streams = NULL;
    pthread_mutex_unlock(&stream_lock);
}
