#include <stdlib.h>
#include <stdio.h>
#include <envex.h>

#include "sdrs.h"
#include "test.h"
#include "logging.h"

#define LOGEX_TAG "B210-TEST"
#include <logex-main.h>

static int
read_test()
{
    int ret = 1;

    double freq = 1000000000.0;
    double samp_rate = 30720000.0;
    double bandwidth = samp_rate;

    size_t bytes = (size_t)(samp_rate * .01);
    char *buf = malloc(bytes);

    IO_HANDLE b210 = new_b210_rx_machine();
    if (b210 == 0) {
        goto do_return;
    }

    b210_set_rx(b210, freq, samp_rate, bandwidth);

    size_t remaining = (size_t)samp_rate;
    while (remaining) {
        size_t b = (bytes < remaining) ? bytes : remaining;
        if (b210_rx_machine->read(b210, buf, &b) < IO_SUCCESS) {
            goto do_return;
        }
        remaining -= b;
    }

    ret = 0;

do_return:
    b210_rx_machine->stop(b210);
    b210_rx_machine->destroy(b210);

    if (buf) {
        free(buf);
    }

    return ret;
}

static int
restart_test()
{
    int ret = 1;

    double freq = 1000000000.0;
    double samp_rate = 30720000.0;
    double bandwidth = samp_rate;

    size_t bytes = (size_t)(samp_rate * .01);
    char *buf = malloc(bytes);

    IO_HANDLE b210 = 0;
    for (int i = 0; i < 2; i++) {
        b210 = new_b210_rx_machine("31A3D1E");
        if (b210 == 0) {
            goto do_return;
        }

        b210_set_rx(b210, freq, samp_rate, bandwidth);

        size_t remaining = (size_t)samp_rate;
        while (remaining) {
            size_t b = (bytes < remaining) ? bytes : remaining;
            if (b210_rx_machine->read(b210, buf, &b) < IO_SUCCESS) {
                goto do_return;
            }
            remaining -= b;
        }

        b210_rx_machine->stop(b210);
        b210_rx_machine->destroy(b210);
        b210 = 0;
    }

    ret = 0;

do_return:
    if (b210 > 0) {
        b210_rx_machine->stop(b210);
        b210_rx_machine->destroy(b210);
    }

    if (buf) {
        free(buf);
    }

    return ret;
}


int
main(int nargs, char *argv[])
{
    char *log_level;
    ENVEX_TOSTR_UPPER(log_level, "BW_TEST_UNIT_LOG_LEVEL", "error");
    set_log_level_str(log_level);

    test_setup();

    test_add(read_test);
    test_add(restart_test);

    test_run();
    test_cleanup();
}
