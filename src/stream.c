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

#define LOGEX_TAG "BW-STREAM"
#include "bw-log.h"

#define STREAM_NAME_LEN 1024

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
    char *name;                 // Stream name
    POOL *pool;                 // Memory pool

    // Segments
    IO_SEGMENT *segments;       // Segments associated with this stream
    int n_segment;              // Number of segments
    int segment_len;            // Segment entries

    enum stream_status_e status;

    struct io_stream_t *next;   // Next stream in list
} *streams = NULL;

static void
set_state(struct io_stream_t *stream, enum stream_state_e new_state)
{
    if (new_state > STREAM_STATE_ERROR) {
        error("Invalid State (%d)", stream->state);
        stream->status = STREAM_ERROR_INVALID_STATE;
        new_state = STREAM_STATE_ERROR;
    }
    
    trace("%s: state change from %s to %s",
        stream->name, STREAM_STATE_PRINT(stream->state), STREAM_STATE_PRINT(new_state));

    stream->state = new_state;
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
    case STREAM_STATE_ERROR:
    case STREAM_FINISHING:
    case STREAM_DONE:
        break;

    default:
        set_state(stream, STREAM_STATE_ERROR);
        break;
    }
    stream->status = STREAM_ERROR;

    pthread_mutex_unlock(&stream->lock);
}

static void *
main_state_machine(void *args)
{
    struct io_stream_t *st = (struct io_stream_t *)args;
    set_state(st, STREAM_READY);

    // Start segments
    int s = 0;
    for (; s < st->n_segment; s++) {
        IO_SEGMENT seg = st->segments[s];
        segment_start(seg, &st->state);
    }

    set_state(st, STREAM_RUNNING);

    // Wait for
    //  1) A segment to signal completion
    //  2) A segment to signal error
    //  3) A shutdown from the main thread
    while (STREAM_RUNNING == st->state) {
        // TODO: Use condition
        usleep(1000);
    }

    // Wait for segments to complete final transactions
    if (STREAM_FINISHING == st->state) {
        s = 0;
        while (s < st->n_segment) {
            IO_SEGMENT seg = st->segments[s];

            if (!segment_is_running(seg)) {
                trace("%s: Segment %d not running", st->name, s);
                s++;
            }
        }
    }

    // Join stream segments
    for (s = 0; s < st->n_segment; s++) {
        IO_SEGMENT seg = st->segments[s];
        segment_join(seg);
        segment_print_metrics(seg);
    }

    set_state(st, STREAM_STOPPED);
    pthread_exit(NULL);
}

static struct io_stream_t *
get_stream(IO_STREAM h)
{
    struct io_stream_t *ret = NULL;

    pthread_mutex_lock(&stream_lock);
    struct io_stream_t *s = streams;
    while (s) {
        if (s->id == h) {
            ret = s;
            break;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&stream_lock);

    return ret;
}

/*
 * Create a new stream struct and add it to the stream list.  Return the stream handle.
 */
IO_STREAM
new_stream()
{
    POOL *p = create_pool();
    struct io_stream_t *stream = pcalloc(p, sizeof(struct io_stream_t));
    pthread_mutex_init(&stream->lock, NULL);

    // Init stream struct
    pthread_mutex_lock(&stream->lock);

    stream->state = STREAM_INIT;
    stream->id = ++stream_counter;
    stream->pool = p;
    stream->segments = NULL;
    stream->n_segment = 0;
    stream->segment_len = 0;
    stream->next = NULL;

    stream->name = pcalloc(p, STREAM_NAME_LEN);
    snprintf(stream->name, STREAM_NAME_LEN-1, "stream%d", stream->id);

    pthread_mutex_unlock(&stream->lock);

    // Add stream to list
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

    return stream->id;
}

static void
add_segment(struct io_stream_t *st, IO_SEGMENT seg)
{
    pthread_mutex_lock(&st->lock);

    char **group = &st->name;
    segment_set_group(seg, group);
    int i = st->n_segment++;
    if (i >= st->segment_len) {
        st->segment_len += 10;
        st->segments = repalloc(st->segments, sizeof(int) * st->segment_len, st->pool);
    }

    st->segments[i] = seg;
    pthread_mutex_unlock(&st->lock);
}

static void
register_callbacks(struct io_stream_t *st, IO_SEGMENT seg)
{
    segment_register_callback_complete(seg, callback_complete, st);
    segment_register_callback_error(seg, callback_error, st);
}

static void
create_segment_1_1(struct io_stream_t *st, IO_HANDLE in, IO_HANDLE out)
{
    IO_SEGMENT s = segment_create_1_1(st->pool, in, out);
    register_callbacks(st, s);
    add_segment(st, s);
}

static void
create_segment_1_2(struct io_stream_t *st, IO_HANDLE in, IO_HANDLE out, IO_HANDLE out1)
{
    IO_SEGMENT s = segment_create_1_2(st->pool, in, out, out1);
    register_callbacks(st, s);
    add_segment(st, s);
}

int
io_stream_add_src_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return 1;
    }

    IO_HANDLE src_buf = 0;
    IO_SEGMENT src_seg = segment_create_src(st->pool, in, &src_buf);
    register_callbacks(st, src_seg);
    add_segment(st, src_seg);

    create_segment_1_1(st, src_buf, out);

    return 0;
}

int
io_stream_add_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return 1;
    }

    create_segment_1_1(st, in, out);

    return 0;
}

int
io_stream_add_tee_segment(IO_STREAM h, IO_HANDLE in, IO_HANDLE out, IO_HANDLE out1)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return 1;
    }

    create_segment_1_2(st, in, out, out1);

    return 0;
}

int
start_stream(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return IO_ERROR;
    }

    // Run the stream as its own thread
    pthread_create(&st->thread, NULL, main_state_machine, (void *)st);
    return IO_SUCCESS;
}

int
join_stream(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return STREAM_ERROR_NOT_FOUND;
    }

    pthread_join(st->thread, NULL);

    return st->status;
}

int
stream_get_status(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return 0;
    }
}

static void
stop_stream_internal(struct io_stream_t *st)
{
    trace("%s: Stopping stream (state == %s)", st->name, STREAM_STATE_PRINT(st->state));
    while (1) {
        switch (st->state) {
        // wait for stream to start running
        case STREAM_INIT:
        case STREAM_READY:
            // TODO: Use condition
            usleep(1000);
            continue;

        // send completion signal to the stream
        case STREAM_RUNNING:
        case STREAM_FINISHING:
            set_state(st, STREAM_DONE);
            return;

        case STREAM_DONE:
            return;

        case STREAM_STATE_ERROR:
            error("%s: Error during STOP command", st->name);
            st->status = STREAM_ERROR_STOP_FAILURE;
            return;

        case STREAM_STOPPED:
            warn("%s: Stream already stopped", st->name);

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
        error("Stream %d not found", h);
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

        int s = 0;
        for (; s < st->n_segment; s++) {
            IO_SEGMENT seg = st->segments[s];
            segment_destroy(seg);
        }
        
        void *destroy_me = st->pool;
        st = st->next;
        free_pool(destroy_me);
    }

    streams = NULL;
    pthread_mutex_unlock(&stream_lock);
}

void
stream_set_name(IO_STREAM h, const char *name)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return;
    }

    snprintf(st->name, STREAM_NAME_LEN-1, "%s", name);
}

void
stream_set_default_buflen(IO_STREAM h, size_t len)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return;
    }

    int s = 0;
    for (; s < st->n_segment; s++) {
        IO_SEGMENT seg = st->segments[s];
        segment_set_default_buflen(seg, len);
    }
}

void
stream_enable_metrics(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return;
    }

    int s = 0;
    for (; s < st->n_segment; s++) {
        IO_SEGMENT seg = st->segments[s];
        segment_enable_metrics(seg);
    }
}

void
stream_print_metrics(IO_STREAM h)
{
    // Get stream from handle
    struct io_stream_t *st = get_stream(h);
    if (!st) {
        error("Stream %d not found", h);
        return;
    }

    int s = 0;
    for (; s < st->n_segment; s++) {
        IO_SEGMENT seg = st->segments[s];
        segment_print_metrics(seg);
    }
}

void
stream_set_log_level(char *level)
{
    bw_set_log_level_str(level);
}
