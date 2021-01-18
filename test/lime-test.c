#include <stdlib.h>
#include <stdio.h>
#include <envex.h>

#include "simple-machines.h"
#include "sdrs.h"
#include "stream.h"
#include "test.h"
#include "logging.h"

#define LOGEX_TAG "LIME-TEST"
#include <logex-main.h>

static int
restart_test()
{
    int ret = 1;

    double freq = 1000000000.0;
    double samp_rate = 30720000.0;
    double bandwidth = samp_rate;

    size_t bytes = (size_t)(samp_rate * .01);
    char *buf = malloc(bytes);

    IO_HANDLE lime = 0;
    for (int i = 0; i < 2; i++) {
        lime = new_lime_rx_machine();
        lime_set_rx(lime, freq, samp_rate, bandwidth);

        size_t remaining = (size_t)samp_rate;
        while (remaining) {
            size_t b = (bytes < remaining) ? bytes : remaining;
            if (lime_rx_machine->read(lime, buf, &b) < IO_SUCCESS) {
                goto do_return;
            }
            remaining -= b;
        }

        lime_rx_machine->stop(lime);
        lime_rx_machine->destroy(lime);
        lime = 0;
    }

    ret = 0;

do_return:
    if (lime > 0) {
        lime_rx_machine->stop(lime);
        lime_rx_machine->destroy(lime);
    }

    if (buf) {
        free(buf);
    }

    return ret;
}

static int
read_test()
{
    int ret = 1;

    double freq = 1000000000.0;
    double samp_rate = 30720000.0;
    double bandwidth = samp_rate;

    size_t bytes = (size_t)(samp_rate * .01);
    char *buf = malloc(bytes);

    IO_HANDLE lime = new_lime_rx_machine();
    lime_set_rx(lime, freq, samp_rate, bandwidth);

    size_t remaining = (size_t)samp_rate;
    while (remaining) {
        size_t b = (bytes < remaining) ? bytes : remaining;
        if (lime_rx_machine->read(lime, buf, &b) < IO_SUCCESS) {
            goto do_return;
        }
        remaining -= b;
    }

    ret = 0;

do_return:
    lime_rx_machine->stop(lime);
    lime_rx_machine->destroy(lime);

    if (buf) {
        free(buf);
    }

    return ret;
}

static int
stream_test()
{
    int ret = 1;

    double freq = 1000000000.0;
    double samp_rate = 30720000.0;
    double bandwidth = samp_rate;

    size_t bytes = (size_t)(samp_rate * .01);
    char *buf = malloc(bytes);

    IO_HANDLE lime = new_lime_rx_machine();
    lime_set_rx(lime, freq, samp_rate, bandwidth);

    IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test", "cfile");

    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out, BW_NOFLAGS);

    size_t remaining = (size_t)samp_rate;
    while (remaining) {
        size_t b = (bytes < remaining) ? bytes : remaining;
        if (lime_rx_machine->read(lime, buf, &b) < IO_SUCCESS) {
            goto do_return;
        }
        remaining -= b;
    }

    ret = 0;

do_return:
    lime_rx_machine->stop(lime);
    lime_rx_machine->destroy(lime);

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

    lime_set_log_level("warn"); 

    test_setup();

    test_add(restart_test);
    test_add(read_test);
    test_add(stream_test);

    test_run();
    test_cleanup();
}
