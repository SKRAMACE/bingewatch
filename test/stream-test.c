#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <uuid/uuid.h>
#include <memex.h>

#include "simple-machines.h"
#include "simple-buffers.h"
#include "simple-filters.h"
#include "stream.h"

static char *rootdir = NULL;

static void
fmt_rootdir(char *root)
{
    if (!rootdir) {
        rootdir = malloc(1024);
    }

    uuid_t binuuid;
    uuid_generate_random(binuuid);

    char uuid[37];
    uuid_unparse(binuuid, uuid);

    snprintf(rootdir, 1024, "%s/%s-%s", root, "bingewatch-test", uuid);
}

static size_t
fill_float_data(size_t len, float **data)
{
    size_t bytes = len * sizeof(float);
    float *x = malloc(bytes);

    int i = 0;
    for (; i < 100; i++) {
        x[i] = (float)i;
    }

    *data = x;
    return bytes;
}

static int
run_stream_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char *infile = "stream_test_data";
    char *outfile = "stream_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_machine(rootdir, infile, "float", FFILE_RW);
    IO_HANDLE out = new_file_machine(rootdir, outfile, "float", FFILE_WRITE);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, out, BW_NOFLAGS);

    // Fill in file with data
    size_t b = bytes;
    file_machine->write(in, data, &b);
    if (b != bytes) {
        printf("\tFAIL: Write byte mismatch\n");
        goto do_return;
    }

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(bytes);
    b = bytes;
    file_machine->read(out, rdata, &b);
    if (b != bytes) {
        printf("\tFAIL: Read byte mismatch\n");
        goto do_return;
    }

    if (memcmp(data, rdata, bytes) != 0) {
        printf("\tFAIL: Data mismatch\n");
        goto do_return;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
run_buffered_stream_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char *infile = "buffered_stream_test_data";
    char *outfile = "buffered_stream_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_machine(rootdir, infile, "float", FFILE_RW);
    IO_HANDLE out = new_file_machine(rootdir, outfile, "float", FFILE_WRITE);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, out, BW_NOFLAGS);

    // Fill in file with data
    size_t b = bytes;
    file_machine->write(in, data, &b);
    if (b != bytes) {
        printf("\tFAIL: Write byte mismatch\n");
        goto do_return;
    }

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(bytes);
    b = bytes;
    file_machine->read(out, rdata, &b);
    if (b != bytes) {
        printf("\tFAIL: Read byte mismatch\n");
        goto do_return;
    }

    if (memcmp(data, rdata, bytes) != 0) {
        printf("\tFAIL: Data mismatch\n");
        goto do_return;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
run_multisegment_stream_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char *infile = "buffered_stream_test_data";
    char *outfile = "buffered_stream_test_out";
    char *rdata = NULL;

    // Create file machines
    IO_HANDLE in = new_file_machine(rootdir, infile, "float", FFILE_RW);
    IO_HANDLE out = new_file_machine(rootdir, outfile, "float", FFILE_WRITE);

    IO_HANDLE buf1;
    IO_HANDLE buf2;
    new_rb_machine(&buf1, 100, 20);
    new_rb_machine(&buf2, 100, 20);

    // Create stream
    IO_STREAM stream = new_stream();
    io_stream_add_segment(stream, in, buf1, BW_NOFLAGS);
    io_stream_add_segment(stream, buf1, buf2, BW_NOFLAGS);
    io_stream_add_segment(stream, buf2, out, BW_NOFLAGS);

    // Fill in file with data
    size_t b = bytes;
    file_machine->write(in, data, &b);
    if (b != bytes) {
        printf("\tFAIL: Write byte mismatch\n");
        goto do_return;
    }

    start_stream(stream);
    join_stream(stream);

    //file_machine->destroy(in);
    //file_machine->destroy(out);

    // Read and compare
    out = new_file_machine(rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(bytes);
    b = bytes;
    file_machine->read(out, rdata, &b);
    if (b != bytes) {
        printf("\tFAIL: Read byte mismatch\n");
        goto do_return;
    }

    if (memcmp(data, rdata, bytes) != 0) {
        printf("\tFAIL: Data mismatch\n");
        goto do_return;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}

static int
run_byte_count_stream_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char *outfile = "byte_count_stream_test_out";
    char *rdata = NULL;
    size_t limit_bytes = 1024 * 1024;

    // Create file machines
    IO_HANDLE in = new_file_read_machine("/dev/urandom");
    IO_HANDLE out = new_file_machine(rootdir, outfile, "float", FFILE_WRITE);

    IO_HANDLE buf1;
    IO_HANDLE buf2;
    new_rb_machine(&buf1, 100, 20);
    new_rb_machine(&buf2, 100, 20);

    // Create byte counter
    POOL *p = create_pool();
    struct io_filter_t *limiter = create_byte_count_limit_filter(p, "limiter", limit_bytes);
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
    out = new_file_machine(rootdir, outfile, "float", FFILE_READ);
    rdata = malloc(limit_bytes);

    size_t b = limit_bytes;
    file_machine->read(out, rdata, &b);
    if (b != limit_bytes) {
        printf("\tFAIL: Read byte mismatch\n");
        goto do_return;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    if (rdata) {
        free(rdata);
    }

    return ret;
}


int
main(int nargs, char *argv[])
{
    stream_set_log_level("trace");
    fmt_rootdir("/tmp");
    printf("outdir: %s\n", rootdir);
    float *data;
    size_t bytes = fill_float_data(100, &data);

    run_stream_test(data, bytes);
    run_buffered_stream_test(data, bytes);
    run_multisegment_stream_test(data, bytes);
    run_byte_count_stream_test(data, bytes);

    if (data) {
        free(data);
    }

    if (rootdir) {
        free(rootdir);
    }
}
