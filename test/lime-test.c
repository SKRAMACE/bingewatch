#include <stdlib.h>
#include <stdio.h>
#include <envex.h>
#include <memex.h>
#include <complex.h>

#include "sdrs.h"
#include "simple-machines.h"
#include "simple-filters.h"
#include "stream.h"
#include "test.h"
#include "logging.h"

#define LOGEX_TAG "LIME-TEST"
#include <logex-main.h>

static struct {
    double freq;
    double samp_rate;
    double bandwidth;
} defaults = {1000000000.0, 30720000.0, 30720000.0};

typedef struct lime_gain_model_t {
    struct bw_gain_model_t _gm;
    float lna;
    float pga;
    float tia;
} LIME_GAIN; 

static inline int
gains_match(GAIN_MODEL *model, float lna, float pga, float tia)
{
    LIME_GAIN *x = (LIME_GAIN *)model;
    return (x->lna != lna || x->pga != pga || x->tia != tia) ? 0 : 1;
}

static inline IO_HANDLE
lime_setup_vars(double freq, double samp_rate, double bandwidth)
{
    IO_HANDLE lime = new_lime_rx_machine();
    lime_set_rx(lime, freq, samp_rate, bandwidth);
    return lime;
}

static inline IO_HANDLE
lime_setup()
{
    return lime_setup_vars(defaults.freq, defaults.samp_rate, defaults.bandwidth);
}

static inline void
lime_teardown(IO_HANDLE lime)
{
    if (lime > 0) {
        lime_rx_machine->stop(lime);
        lime_rx_machine->destroy(lime);
    }
}

static int
gain_test()
{
    int ret = 1;

    IO_HANDLE lime = lime_setup();
    POOL *pool = create_pool();

    GAIN_MODEL *check = lime_rx_gain_model_init(pool);
    GAIN_MODEL *model = lime_rx_gain_model_init(pool);
    lime_rx_set_gain_model(lime, model);
    lime_rx_get_gain_model(lime, check);

    if (!gains_match(check, 0, 0, 0)) {
        error("Gain model not zeros");
        goto do_return;
    }

    if (lime_rx_gain_dec(model) != 1) {
        error("Decrease below zero");
        goto do_return;
    }

    while (lime_rx_gain_inc(model) == 0) {
        continue;
    }

    if (!gains_match(model, 30, 19, 0)) {
        LIME_GAIN *m = (LIME_GAIN *)model;
        error("Max mismatch: %0.2f, %0.2f, %0.2f", m->lna, m->pga, m->tia);
        goto do_return;
    }

    LIME_GAIN *m = (LIME_GAIN *)model;
    m->lna = 3;
    m->pga = 6;
    lime_rx_set_gain_model(lime, model);
    lime_rx_get_gain_model(lime, check);

    if (!gains_match(check, 3, 6, 0)) {
        error("Gain model not set");
        goto do_return;
    }
    ret = 0;

    lime_rx_get_gain_model(lime, model);
    lime_rx_get_gain_model(lime, check);
    if (memcmp(model, check, model->len != 0)) {
        error("Gain model memcmp failure");
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);
    return ret;
}

static int
restart_test()
{
    int ret = 1;

    size_t n_samp = (size_t)(defaults.samp_rate * .01);
    size_t bytes = n_samp * sizeof(float complex);
    char *buf = malloc(bytes);

    IO_HANDLE lime = 0;
    for (int i = 0; i < 2; i++) {
        lime = lime_setup();

        size_t remaining = (size_t)defaults.samp_rate;
        while (remaining) {
            size_t n = (n_samp < remaining) ? n_samp : remaining;
            size_t b = n * sizeof(float complex);
            if (lime_rx_machine->read(lime, buf, &b) < IO_SUCCESS) {
                goto do_return;
            }
            remaining -= n;
        }

        lime_teardown(lime);
    }

    ret = 0;

do_return:
    lime_teardown(lime);

    if (buf) {
        free(buf);
    }

    return ret;
}

static int
read_test()
{
    int ret = 1;

    size_t n_samp = (size_t)(defaults.samp_rate * .01);
    size_t bytes = n_samp * sizeof(float complex);
    char *buf = malloc(bytes);

    IO_HANDLE lime = lime_setup();

    size_t remaining = (size_t)defaults.samp_rate;
    while (remaining) {
        size_t n = (n_samp < remaining) ? n_samp : remaining;
        size_t b = n * sizeof(float complex);
        if (lime_rx_machine->read(lime, buf, &b) < IO_SUCCESS) {
            goto do_return;
        }
        remaining -= n;
    }

    ret = 0;

do_return:
    lime_teardown(lime);

    if (buf) {
        free(buf);
    }

    return ret;
}

static int
stream_test()
{
    int ret = 1;

    size_t n_samp = (size_t)(defaults.samp_rate * .01);
    size_t bytes = n_samp * sizeof(float complex);
    char *buf = malloc(bytes);

    IO_HANDLE lime = lime_setup();

    IO_HANDLE out = new_null_machine();
    //IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test", "cfile");

    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t remaining = (size_t)defaults.samp_rate;
    while (remaining) {
        size_t n = (n_samp < remaining) ? n_samp : remaining;
        size_t b = n * sizeof(float complex);
        if (lime_rx_machine->read(lime, buf, &b) < IO_SUCCESS) {
            goto do_return;
        }
        remaining -= n;
    }

    ret = 0;

do_return:
    lime_teardown(lime);

    if (buf) {
        free(buf);
    }

    return ret;
}

static int
overflow_test()
{
    int ret = 1;

    IO_HANDLE lime = lime_setup();
    IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test", "cfile");
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t bytes_per_second = (size_t)(defaults.samp_rate * sizeof(float complex));
    size_t seconds = 1;
    POOL *pool = create_pool();
    IO_FILTER *counter = create_byte_count_limit_filter(pool, "counter", bytes_per_second * seconds);

    start_stream(stream);
    if (join_stream(stream) == 0) {
        ret = 0;
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);

    return ret;
}

int
main(int nargs, char *argv[])
{
    critical("THIS TEST IS OUT OF DATE, AND NEEDS DEVELOPER REVIEW");

    char *log_level;
    ENVEX_TOSTR_UPPER(log_level, "BW_TEST_UNIT_LOG_LEVEL", "error");
    set_log_level_str(log_level);

    lime_set_log_level("info"); 

    test_setup();

    test_add(gain_test);
    test_add(restart_test);
    test_add(read_test);
    test_add(stream_test);
    test_add(overflow_test);

    test_run();
    test_cleanup();
}
