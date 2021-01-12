#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>
#include <memex.h>

#include "simple-machines.h"
#include "simple-buffers.h"
#include "simple-filters.h"
#include "logging.h"
#include "stream.h"
#include "test.h"

#define LOGEX_TAG "STREAM-TEST"
#include <logex-main.h>

static int
stream_test()
{
    int ret = 1;

    float *data;
    size_t bytes = fill_float_data(100, &data);

    char *infile = "stream_test_data";
    char *outfile = "stream_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_machine(bw_test_rootdir, infile, "float", FFILE_RW);
    IO_HANDLE out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_WRITE);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, out, BW_NOFLAGS);

    // Fill in file with data
    size_t b = bytes;
    file_machine->write(in, data, &b);
    if (b != bytes) {
        goto do_return;
    }

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(bytes);
    b = bytes;
    file_machine->read(out, rdata, &b);
    if (b != bytes) {
        goto do_return;
    }

    if (memcmp(data, rdata, bytes) != 0) {
        goto do_return;
    }

    ret = 0;

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
buffered_stream_test()
{
    int ret = 1;

    float *data;
    size_t bytes = fill_float_data(100, &data);

    char *infile = "buffered_stream_test_data";
    char *outfile = "buffered_stream_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_machine(bw_test_rootdir, infile, "float", FFILE_RW);
    IO_HANDLE out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_WRITE);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, out, BW_NOFLAGS);

    // Fill in file with data
    size_t b = bytes;
    file_machine->write(in, data, &b);
    if (b != bytes) {
        goto do_return;
    }

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(bytes);
    b = bytes;
    file_machine->read(out, rdata, &b);
    if (b != bytes) {
        goto do_return;
    }

    if (memcmp(data, rdata, bytes) != 0) {
        goto do_return;
    }

    ret = 0;

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
multisegment_stream_test()
{
    int ret = 1;

    float *data;
    size_t bytes = fill_float_data(100, &data);

    char *infile = "buffered_stream_test_data";
    char *outfile = "buffered_stream_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_machine(bw_test_rootdir, infile, "float", FFILE_RW);
    IO_HANDLE out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_WRITE);

    IO_HANDLE buf1 = new_rb_machine();
    IO_HANDLE buf2 = new_rb_machine();

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, buf1, BW_NOFLAGS);
    io_stream_add_segment(stream, buf1, buf2, BW_NOFLAGS);
    io_stream_add_segment(stream, buf2, out, BW_NOFLAGS);

    // Fill in file with data
    size_t b = bytes;
    file_machine->write(in, data, &b);
    if (b != bytes) {
        goto do_return;
    }

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(bytes);
    b = bytes;
    file_machine->read(out, rdata, &b);
    if (b != bytes) {
        goto do_return;
    }

    if (memcmp(data, rdata, bytes) != 0) {
        goto do_return;
    }

    ret = 0;

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
byte_count_stream_test()
{
    int ret = 1;

    float *data;
    size_t bytes = fill_float_data(100, &data);

    char *outfile = "byte_count_stream_test_out";
    char *rdata = NULL;
    size_t limit_bytes = 1024 * 1024 * 100;

    // Create file machines
    IO_HANDLE in = new_file_read_machine("/dev/urandom");
    IO_HANDLE out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_WRITE);

    IO_HANDLE buf1 = new_rb_machine();
    IO_HANDLE buf2 = new_rb_machine();

    // Create byte counter
    POOL *p = create_pool();
    IO_FILTER *limiter = create_byte_count_limit_filter(p, "limiter", limit_bytes);
    add_write_filter(buf1, limiter);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, buf1, BW_NOFLAGS);
    io_stream_add_segment(stream, buf1, buf2, BW_NOFLAGS);
    io_stream_add_segment(stream, buf2, out, BW_NOFLAGS);

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(limit_bytes);

    size_t b = limit_bytes;
    file_machine->read(out, rdata, &b);
    if (b != limit_bytes) {
        goto do_return;
    }

    ret = 0;

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
stream_metrics_test()
{
    int ret = 1;

    float *data;
    size_t bytes = fill_float_data(100, &data);

    char *outfile = "run_stream_metrics_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_read_machine("/dev/urandom");
    IO_HANDLE out = new_file_machine(bw_test_rootdir, outfile, "float", FFILE_WRITE);

    IO_HANDLE buf1 = new_rb_machine();
    IO_HANDLE buf2 = new_rb_machine();

    // Create byte counter
    POOL *p = create_pool();
    IO_FILTER *limiter = create_time_limit_filter(p, "limiter", 5000);
    add_write_filter(buf1, limiter);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, buf1, BW_NOFLAGS);
    io_stream_add_segment(stream, buf1, buf2, BW_NOFLAGS);
    io_stream_add_segment(stream, buf2, out, BW_NOFLAGS);

    stream_enable_metrics(stream);

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    ret = 0;

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

int
main(int nargs, char *argv[])
{
    test_init_logging(); 

    test_setup();

    segment_set_log_level("trace");
    test_add(stream_test);
    test_add(buffered_stream_test);
    test_add(multisegment_stream_test);
    test_add(byte_count_stream_test);
    test_add(stream_metrics_test);

    test_run();
    test_cleanup();
}
