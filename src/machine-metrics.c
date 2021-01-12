#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <memex.h>

#define LOGEX_TAG "BW-MACHINE"
#include "bw-log.h"
#include "bw-util.h"

#include "machine.h"

#define METRIC_CALC_ALLOC 10
#define TIMEVAL_US(t) ((double)(t.tv_sec * 1000000) + (double)(t.tv_usec))

static pthread_mutex_t machine_metrics_lock = PTHREAD_MUTEX_INITIALIZER;

typedef void *(*timer_fn)(void*);
enum timer_state_e {
    MM_TIMER_STOPPED,
    MM_TIMER_RUNNING,
};

struct timer_t {
    const char *name;
    size_t ms;
    enum timer_state_e state;
    pthread_t thread;
    pthread_mutex_t lock;
    timer_fn callback;
};

static void *machine_metrics_update_callback(void *args);
static struct timer_t updater = {
    .name="update timer",
    .ms=1000000,
    .state=MM_TIMER_STOPPED,
    .lock=PTHREAD_MUTEX_INITIALIZER,
    .callback=machine_metrics_update_callback,
};

static void *machine_metrics_print_callback(void *args);
static struct timer_t printer = {
    .name="print timer",
    .ms=60000000,
    .state=MM_TIMER_STOPPED,
    .lock=PTHREAD_MUTEX_INITIALIZER,
    .callback=machine_metrics_print_callback,
};

static enum timer_state_e update_timer_state = MM_TIMER_STOPPED;
static enum timer_state_e print_timer_state = MM_TIMER_STOPPED;

static struct machine_metrics_signal_t {
    POOL *pool;
    int **signal;
    uint32_t len;
    uint32_t size;
} global_signal = {NULL, NULL, 0, 0};

static void *
run_timer(void *args)
{
    struct timer_t *timer = (struct timer_t *)args;

    pthread_mutex_lock(&timer->lock);
    size_t ms_reset = timer->ms;
    pthread_mutex_unlock(&timer->lock);

    size_t ms = 0;
    while (timer->state == MM_TIMER_RUNNING) {
        if (ms++ < ms_reset) {
            goto wait_1ms;
        }

        ms = ms_reset;
        timer->callback(timer);

    wait_1ms:
        usleep(1000);
    }

    free(args);
}

static void *
machine_metrics_update_callback(void *args)
{
    pthread_mutex_lock(&machine_metrics_lock);
    for (int i = 0; i < global_signal.len; i++) {
        *global_signal.signal[i] = 1;
    }
    pthread_mutex_unlock(&machine_metrics_lock);
}

static void *
machine_metrics_print_callback(void *args)
{
    pthread_mutex_lock(&machine_metrics_lock);
    for (int i = 0; i < global_signal.len; i++) {
        *global_signal.signal[i] = 1;
    }
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

    g->signal[g->len++] = &m->_update_signal;
    pthread_mutex_unlock(&machine_metrics_lock);
}

static void
machine_metrics_update_fn(void *metrics, size_t req_bytes, size_t rec_bytes)
{
    IO_METRICS *m = (IO_METRICS *)metrics;

    pthread_mutex_lock(&m->lock);
    m->req.count++;
    m->req.bytes += req_bytes;

    if (rec_bytes > 0) {
        m->rec.count++;
        m->rec.bytes += rec_bytes;
    }

    memcpy(&m->t_prev, &m->t_cur, sizeof(struct timeval));
    gettimeofday(&m->t_cur, NULL);
    memcpy(&m->t_stop, &m->t_cur, sizeof(struct timeval));
    pthread_mutex_unlock(&m->lock);

    if (m->_update_signal) {
        machine_metrics_update(m);
    }

    if (m->_print_signal) {
        machine_metrics_print(m);
    }
}

static void
machine_metrics_init(void *metrics, size_t req_bytes, size_t rec_bytes)
{
    IO_METRICS *m = (IO_METRICS *)metrics;

    pthread_mutex_lock(&m->lock);
    gettimeofday(&m->t_cur, NULL);
    memcpy(&m->t_start, &m->t_cur, sizeof(struct timeval));
    m->fn = machine_metrics_update_fn;
    pthread_mutex_unlock(&m->lock);

    machine_metrics_update_fn(metrics, req_bytes, rec_bytes);
}

struct io_metrics_t *
machine_metrics_create(POOL *pool)
{
    struct io_metrics_t *m = (struct io_metrics_t *)pcalloc(pool, sizeof(struct io_metrics_t));
    m->in.pool = pool;
    m->out.pool = pool;

    pthread_mutex_init(&m->in.lock, NULL);
    pthread_mutex_init(&m->out.lock, NULL);

    m->in.fn = machine_metrics_init;
    m->out.fn = machine_metrics_init;

    track_new_metric(&m->in);
    track_new_metric(&m->out);

    return m;
}

static void
start_timer(size_t ms, struct timer_t *timer)
{
    pthread_mutex_lock(&timer->lock);
    enum timer_state_e state = timer->state;
    pthread_mutex_unlock(&timer->lock);

    if (MM_TIMER_STOPPED == state) {
        pthread_mutex_lock(&timer->lock);
        timer->ms = ms;
        timer->state = MM_TIMER_RUNNING;
        pthread_mutex_unlock(&timer->lock);

        pthread_create(&timer->thread, NULL, run_timer, (void *)timer);

    } else {
        warn("Failed to start timer \"%s\" (already running)", timer->name);
    }
}

static void
stop_timer(struct timer_t *timer)
{
    pthread_mutex_lock(&timer->lock);
    enum timer_state_e state = timer->state;
    pthread_mutex_unlock(&timer->lock);

    if (MM_TIMER_STOPPED == state) {
        pthread_mutex_lock(&timer->lock);
        timer->state = MM_TIMER_STOPPED;
        pthread_mutex_unlock(&timer->lock);

        pthread_join(timer->thread, NULL);
    } else {
        warn("Failed to stop timer \"%s\" (not running)", timer->name);
    }
}

void
machine_metrics_update(IO_METRICS *m)
{
    pthread_mutex_lock(&m->lock);

    if (m->n_calc <= m->calc_len) {
        m->calc_len += METRIC_CALC_ALLOC;
        size_t bytes = sizeof(struct io_metrics_calc_t) * m->calc_len;
        m->calc = (struct io_metrics_calc_t *)repalloc(m->calc, bytes, m->pool);
    }
    struct io_metrics_calc_t *mc = m->calc + m->n_calc; 

    memcpy(&mc->time, &m->t_cur, sizeof(struct timeval));
    double t0_us = TIMEVAL_US(m->t_prev);
    double t1_us = TIMEVAL_US(m->t_cur);
    double sec = (t1_us - t0_us) / 1000000;
    mc->elapsed = sec;
    mc->data_rate = (double)m->rec.total_bytes / sec;
    mc->req_rate = (double)m->req.count / sec;
    mc->avg_req_size = (double)m->req.bytes / (double)m->req.count;
    mc->avg_rec_size = (double)m->rec.bytes / (double)m->rec.count;
    mc->utilization = (double)m->rec.count / (double)m->req.count;
    //mc->fill_level = m->req.total - m->rec.total;

    // Update totals
    m->req.total_count += m->req.count;
    m->req.total_bytes += m->req.bytes;
    m->rec.total_count += m->rec.count;
    m->rec.total_bytes += m->rec.bytes;

    // Clear counters
    m->req.count = 0;
    m->req.bytes = 0;
    m->rec.count = 0;
    m->rec.bytes = 0;

    m->_update_signal = 0;

    pthread_mutex_unlock(&m->lock);
}

static void
metrics_inst(IO_METRICS *m, struct io_metrics_calc_t *m0)
{
    pthread_mutex_lock(&m->lock);

    // Copy most recent calculation
    struct io_metrics_calc_t *mc = m->calc + m->n_calc;
    memcpy(m0, mc, sizeof(struct io_metrics_calc_t));

    pthread_mutex_unlock(&m->lock);
}

static void
metrics_avg(IO_METRICS *m, size_t n, struct io_metrics_calc_t *m0)
{
    pthread_mutex_lock(&m->lock);

    // Average the n most recent samples

    pthread_mutex_unlock(&m->lock);
}

static void
metrics_full(IO_METRICS *m, struct io_metrics_calc_t *m0)
{
    pthread_mutex_lock(&m->lock);

    gettimeofday(&m0->time, NULL);

    // Calculate elapsed time in seconds
    double t0_us = TIMEVAL_US(m->t_start);
    double t1_us = TIMEVAL_US(m0->time);
    double sec = (t1_us - t0_us) / 1000000;
    m0->elapsed = sec;
    m0->total_bytes = m->rec.total_bytes;

    // Calculate metrics
    m0->data_rate = (double)m->rec.total_bytes / sec;
    m0->req_rate = (double)m->req.total_count / sec;
    m0->avg_req_size = (double)m->req.total_bytes / (double)m->req.total_count;
    m0->avg_rec_size = (double)m->rec.total_bytes / (double)m->rec.total_count;
    m0->utilization = (double)m->rec.total_count / (double)m->req.total_count;

    pthread_mutex_unlock(&m->lock);
}

static void
machine_metrics_calculate(IO_METRICS *m, struct io_metrics_calc_t *m0, int flag)
{
    if (m->n_calc == 0) {
        machine_metrics_update(m);
    }

    switch (flag & METRICS_CALC_FLAG) {
    case METRICS_CALC_TYPE_INST:
        metrics_inst(m, m0);
        break;

    case METRICS_CALC_TYPE_AVG:
        metrics_avg(m, 10, m0);
        break;

    case METRICS_CALC_TYPE_FULL:
        metrics_full(m, m0);
        break;

    default:
        metrics_inst(m, m0);
        break;
    }
}

size_t
machine_metrics_fmt(IO_METRICS *m, char *buf, size_t len, int flag)
{
    struct io_metrics_calc_t m0;
    memset(&m0, 0, sizeof(struct io_metrics_calc_t));
    machine_metrics_calculate(m, &m0, flag);

    char bytes_str[64];
    size_t_fmt(bytes_str, 64, m0.total_bytes);

    char rate_str[64];
    double_fmt(rate_str, 64, m0.data_rate);

    char reqsz_str[64];
    size_t_fmt(reqsz_str, 64, m0.avg_req_size);

    char recsz_str[64];
    size_t_fmt(recsz_str, 64, m0.avg_rec_size);

    size_t ret = 0;
    switch (flag & METRICS_FMT_FLAG) {
    case METRICS_FMT_TYPE_ONELINE:
        ret = snprintf(buf, len, "%sB %sB/s @ %0.2f utilization",
            bytes_str, rate_str, m0.utilization);
        break;

    default:
        ret = snprintf(buf, len,
            "\t%sB\n"
            "\t%sB/s\n"
            "\t%f requests/sec\n"
            "\t%sB/request\n"
            "\t%sB/receive\n"
            "\t%f utilization\n",
            bytes_str,
            rate_str,
            m0.req_rate,
            reqsz_str,
            recsz_str,
            m0.utilization
        );
    }

    pthread_mutex_unlock(&m->lock);

    return ret;
}


void
machine_metrics_print(IO_METRICS *m)
{
    char buf[1024];
    machine_metrics_fmt(m, buf, 1024, 0);
    printf("%s\n", buf);
}

void
machine_metrics_start(size_t ms)
{
    start_timer(ms, &updater);
}

void
machine_metrics_stop()
{
    stop_timer(&updater);
}

void
machine_metrics_print_start(size_t ms)
{
    start_timer(ms, &printer);
}

void
machine_metrics_print_stop()
{
    stop_timer(&printer);
}
