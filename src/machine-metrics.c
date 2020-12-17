#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <memex.h>

#define LOGEX_TAG "BW-MACHINE"
#include "bw-log.h"

#include "machine.h"

static pthread_mutex_t machine_metrics_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_t timer;

static enum timer_state_e {
    MM_TIMER_STOPPED,
    MM_TIMER_RUNNING,
} timer_state = MM_TIMER_STOPPED;

struct mmgt_t {
    size_t ms;
    enum timer_state_e *state;
};

static struct machine_metrics_signal_t {
    POOL *pool;
    int **signal;
    uint32_t len;
    uint32_t size;
} global_signal = {NULL, NULL, 0, 0};

static void *
machine_metrics_global_timer(void *args)
{
    struct mmgt_t *a = (struct mmgt_t *)args;

    size_t ms_reset = a->ms;
    size_t ms = 0;
    while (*a->state == MM_TIMER_RUNNING) {
        if (ms++ < ms_reset) {
            goto wait_1ms;
        }

        ms = ms_reset;
        pthread_mutex_lock(&machine_metrics_lock);
        for (int i = 0; i < global_signal.len; i++) {
            *global_signal.signal[i] = 1;
        }
        pthread_mutex_unlock(&machine_metrics_lock);

    wait_1ms:
        usleep(1000);
    }

    free(args);
}

static void
start_timer()
{
    pthread_mutex_lock(&machine_metrics_lock);
    timer_state = MM_TIMER_STOPPED;
    pthread_mutex_unlock(&machine_metrics_lock);
}

static void
stop_timer()
{
    pthread_mutex_lock(&machine_metrics_lock);
    timer_state = MM_TIMER_STOPPED;
    pthread_mutex_unlock(&machine_metrics_lock);
}

static void
track_new_metric(IO_METRICS *m)
{
    static struct machine_metrics_signal_t *g = &global_signal;

    pthread_mutex_lock(&machine_metrics_lock);
    if (!g->pool) {
        g->pool = create_pool();
    }

    if (g->len >= g->size) {
        g->size += 10;
        g->signal = repalloc(g->signal, g->size * sizeof(int *), g->pool);
    }

    g->signal[g->len++] = &m->_timer_signal;
    pthread_mutex_unlock(&machine_metrics_lock);
}

static void
machine_metrics_update(void *metrics, size_t bytes)
{
    IO_METRICS *m = (IO_METRICS *)metrics;

    pthread_mutex_lock(&m->lock);
    m->total_bytes += bytes;
    gettimeofday(&m->t_stop, NULL);
    pthread_mutex_unlock(&m->lock);

    if (m->_timer_signal) {
        pthread_mutex_lock(&m->lock);
        int i0 = m->time_i++ % METRIC_HIST;
        int i1 = m->time_i % METRIC_HIST;
        m->bytes[i1] = m->total_bytes - m->bytes[i0];
        memcpy(m->time + i1, &m->t_stop, sizeof(struct timeval));
        gettimeofday(m->time + i1, NULL);
        pthread_mutex_unlock(&m->lock);
    }
}

static void
machine_metrics_init(void *metrics, size_t bytes)
{
    IO_METRICS *m = (IO_METRICS *)metrics;

    pthread_mutex_lock(&m->lock);
    gettimeofday(&m->t_start, NULL);
    m->fn = machine_metrics_update;
    pthread_mutex_unlock(&m->lock);

    machine_metrics_update(metrics, bytes);
}

struct io_metrics_t *
machine_metrics_create(POOL *pool)
{
    struct io_metrics_t *m = (struct io_metrics_t *)pcalloc(pool, sizeof(struct io_metrics_t));

    pthread_mutex_init(&m->in.lock, NULL);
    pthread_mutex_init(&m->out.lock, NULL);

    m->in.fn = machine_metrics_init;
    m->out.fn = machine_metrics_init;

    track_new_metric(&m->in);
    track_new_metric(&m->out);

    return m;
}

void
machine_metrics_timer_start(size_t ms)
{
    pthread_mutex_lock(&machine_metrics_lock);
    enum timer_state_e state = timer_state;
    pthread_mutex_unlock(&machine_metrics_lock);

    if (MM_TIMER_STOPPED == state) {
        start_timer();

        struct mmgt_t *args = (struct mmgt_t *)malloc(sizeof(struct mmgt_t));
        args->ms = ms;
        args->state = &timer_state;
        pthread_create(&timer, NULL, machine_metrics_global_timer, (void *)args);

    } else {
        error("Failed to start machine metrics timer (already running)");
    }
}

void
machine_metrics_timer_stop()
{
    pthread_mutex_lock(&machine_metrics_lock);
    enum timer_state_e state = timer_state;
    pthread_mutex_unlock(&machine_metrics_lock);

    if (MM_TIMER_RUNNING == state) {
        stop_timer();
        pthread_join(timer, NULL);
    } else {
        warn("Failed to stop machine metrics timer (not running)");
    }
}
