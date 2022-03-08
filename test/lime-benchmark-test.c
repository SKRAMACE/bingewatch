#include <envex.h>
#include <memex.h>
#include <complex.h>

#include "lime-test.h"
#include "sdrs.h"
#include "simple-machines.h"
#include "simple-buffers.h"
#include "simple-filters.h"
#include "stream.h"
#include "test.h"
#include "logging.h"

#define LOGEX_TAG "LIME-TEST"
#include <logex-main.h>

static struct lime_params_t *defaults = NULL;

static int
overflow_test_time_limit_1sec()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
    IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test-1sec", "cfile");
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t bytes_per_second = (size_t)(defaults->samp_rate * sizeof(float complex));
    size_t seconds = 1;
    POOL *pool = create_pool();
    IO_FILTER *counter = create_time_limit_filter(pool, "counter", seconds * 1000);
    add_write_filter(out, counter);

    start_stream(stream);
    if (join_stream(stream) == 0) {
        ret = 0;
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);

    return ret;
}

static int
overflow_test_byte_limit_1sec()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
    IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test-1sec", "cfile");
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t bytes_per_second = (size_t)(defaults->samp_rate * sizeof(float complex));
    size_t seconds = 1;
    POOL *pool = create_pool();
    IO_FILTER *counter = create_byte_count_limit_filter(pool, "counter", bytes_per_second * seconds);
    add_write_filter(out, counter);

    start_stream(stream);
    if (join_stream(stream) == 0) {
        ret = 0;
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);

    return ret;
}

static int
buffer_test()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
    //sdrrx_enable_buffering_rate(lime, defaults->samp_rate);
    sdrrx_allow_overruns(lime);
    IO_HANDLE buf = new_rb_machine();

    IO_STREAM stream = new_stream();
    io_stream_add_src_segment(stream, lime, buf);

    size_t bytes_per_second = (size_t)(defaults->samp_rate * sizeof(float complex));
    stream_set_default_buflen(buf, bytes_per_second);
    size_t seconds = 1;
    size_t bytes = bytes_per_second * seconds;

    POOL *pool = create_pool();
    IO_FILTER *counter = create_byte_count_limit_filter(pool, "counter", bytes);
    add_read_filter(buf, counter);

    char *x = palloc(pool, 10 * bytes_per_second);
    start_stream(stream);

    int r;
    do {
        size_t bytes = bytes_per_second / 100;
        r = rb_machine->read(buf, x, &bytes);
    } while (r == IO_SUCCESS);

    if (join_stream(stream) == 0) {
        ret = 0;
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);

    return ret;
}

static int
watermark_test()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
    //sdrrx_enable_buffering_rate(lime, defaults->samp_rate);
    sdrrx_allow_overruns(lime);
    IO_HANDLE buf = new_rb_machine();

    IO_STREAM stream = new_stream();
    io_stream_add_src_segment(stream, lime, buf);

    size_t bytes_per_second = (size_t)(defaults->samp_rate * sizeof(float complex));
    stream_set_default_buflen(buf, bytes_per_second);
    size_t seconds = 1;
    size_t bytes = bytes_per_second * seconds;

    POOL *pool = create_pool();
    IO_FILTER *counter = create_byte_count_limit_filter(pool, "counter", bytes);
    add_read_filter(buf, counter);

    char *x = palloc(pool, 10 * bytes_per_second);
    start_stream(stream);

    int r;
    do {
        size_t bytes = bytes_per_second / 100;
        r = rb_machine->read(buf, x, &bytes);
    } while (r == IO_SUCCESS);

    if (join_stream(stream) == 0) {
        ret = 0;
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);

    return ret;
}


static int
overflow_test_10sec()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
    IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test-10sec", "cfile");
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t bytes_per_second = (size_t)(defaults->samp_rate * sizeof(float complex));
    size_t seconds = 10;
    POOL *pool = create_pool();
    IO_FILTER *counter = create_byte_count_limit_filter(pool, "counter", bytes_per_second * seconds);
    add_write_filter(out, counter);

    start_stream(stream);
    if (join_stream(stream) == 0) {
        ret = 0;
    }

do_return:
    lime_teardown(lime);
    free_pool(pool);

    return ret;
}

static int
overflow_test_10GB()
{
    int ret = 1;

    IO_HANDLE lime = lime_test_setup();
    IO_HANDLE out = new_file_write_machine(bw_test_rootdir, "lime-stream-test-10GB", "cfile");
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, lime, out);

    size_t bytes_per_second = (size_t)(defaults->samp_rate * sizeof(float complex));
    POOL *pool = create_pool();

    size_t bytes = (size_t)10 * GB;
    IO_FILTER *counter = create_byte_count_limit_filter(pool, "counter", bytes);
    add_write_filter(out, counter);

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
    char *log_level;
    ENVEX_TOSTR_UPPER(log_level, "BW_TEST_UNIT_LOG_LEVEL", "error");
    set_log_level_str(log_level);
    lime_set_log_level("info"); 

    defaults = lime_test_get_defaults();
    test_setup();

    //test_add(overflow_test_time_limit_1sec);
    //test_add(overflow_test_byte_limit_1sec);
    test_add(buffer_test);
    //test_add(overflow_test_10sec);
    //test_add(overflow_test_10GB);

    test_run();
    test_cleanup();
}
