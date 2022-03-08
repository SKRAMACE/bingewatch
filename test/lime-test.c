#include <envex.h>
#include <memex.h>
#include <complex.h>

#include "lime-test.h"
#include "sdrs.h"
#include "simple-machines.h"
#include "stream.h"
#include "test.h"
#include "logging.h"

#define LOGEX_TAG "LIME-TEST"
#include <logex-main.h>

static struct lime_params_t *defaults = NULL;

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

static int
gain_test()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
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

    size_t n_samp = (size_t)(defaults->samp_rate * .01);
    size_t bytes = n_samp * sizeof(float complex);
    char *buf = malloc(bytes);

    IO_HANDLE lime = 0;
    for (int i = 0; i < 2; i++) {
        lime = lime_test_setup();

        size_t remaining = (size_t)defaults->samp_rate;
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

    size_t n_samp = (size_t)(defaults->samp_rate * .01);
    size_t bytes = n_samp * sizeof(float complex);
    char *buf = malloc(bytes);

    IO_HANDLE lime = lime_test_setup();

    size_t remaining = (size_t)defaults->samp_rate;
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

    size_t n_samp = (size_t)(defaults->samp_rate * .01);
    size_t bytes = n_samp * sizeof(float complex);
    char *buf = malloc(bytes);

    IO_HANDLE lime = lime_test_setup();

    IO_HANDLE out = new_null_machine();
    //IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test", "cfile");

    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t remaining = (size_t)defaults->samp_rate;
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


int
main(int nargs, char *argv[])
{
    critical("THIS TEST IS OUT OF DATE, AND NEEDS DEVELOPER REVIEW");

    char *log_level;
    ENVEX_TOSTR_UPPER(log_level, "BW_TEST_UNIT_LOG_LEVEL", "error");
    set_log_level_str(log_level);

    lime_set_log_level("info"); 

    defaults = lime_test_get_defaults();
    test_setup();

    test_add(gain_test);
    test_add(restart_test);
    test_add(read_test);
    test_add(stream_test);

    test_run();
    test_cleanup();
}
