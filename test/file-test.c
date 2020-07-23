#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <uuid/uuid.h>

#include "simple-machines.h"

const IOM *m = NULL;

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

static void
gen_timestamp_fmt(char *timestamp, size_t bytes, const char *fmt)
{
    // Get time with microsecond precision
    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Convert seconds to "struct tm" (compatible with strftime)
    time_t nowtime = tv.tv_sec;
    struct tm *nowtm = gmtime(&nowtime);

    char timestr[64];
    strftime(timestr, 64, fmt, nowtm);

    snprintf(timestamp, bytes, "%s", timestr);
}

static void
gen_timestamp(char *timestamp, size_t bytes, const char *fmt)
{
    gen_timestamp_fmt(timestamp, bytes, fmt);
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
run_rw_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char *rdata = NULL;
    IO_HANDLE h = new_file_machine(rootdir, "data", "float", FFILE_RW);

    size_t b = bytes;
    m->write(h, data, &b);

    if (b != bytes) {
        printf("\tFAIL: Write byte mismatch\n");
        goto do_return;
    }

    rdata = malloc(bytes);
    b = bytes;
    m->read(h, rdata, &b);

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
run_read_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char *rdata = NULL;
    char fname[1024];
    snprintf(fname, 1024, "%s/data-read-test", rootdir);
    FILE *f = fopen(fname, "w");
    fwrite(data, 1, bytes, f);
    fclose(f);

    IO_HANDLE h = new_file_read_machine(fname);

    rdata = malloc(bytes);
    size_t b = bytes;
    m->read(h, rdata, &b);

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
run_write_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    char fname[1024];
    snprintf(fname, 1024, "%s/data-write-test.float", rootdir);

    IO_HANDLE h = new_file_write_machine(rootdir, "data-write-test", "float");

    size_t b = bytes;
    m->write(h, data, &b);

    if (b != bytes) {
        printf("\tFAIL: Read byte mismatch\n");
        goto do_return;
    }

    char *rdata = malloc(bytes);
    FILE *f = fopen(fname, "r");
    fread(rdata, 1, bytes, f);
    fclose(f);

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
run_rotate_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    IO_HANDLE h0 = new_file_write_machine(rootdir, "data-rotate-test-auto", "float");
    file_iom_set_auto_rotate(h0);

    IO_HANDLE h1 = new_file_write_machine(rootdir, "data-rotate-test-manual", "float");
    IO_FILTER *touch = file_rotate_filter(h1);

    int i = 0;
    for (i = 0; i < 3; i++) {
        size_t b0 = bytes;
        m->write(h0, data, &b0);

        size_t b1 = bytes;
        m->write(h1, data, &b1);

        if (b0 != bytes || b1 != bytes) {
            printf("\tFAIL: Read byte mismatch\n");
            goto do_return;
        }

        char fname[1024];
        snprintf(fname, 1024, "%s/data-rotate-test-auto-%05d.float", rootdir, i);

        struct stat s;
        if (stat(fname, &s) != 0) {
            printf("\tFAIL: %s not created\n", fname);
            goto do_return;
        }

        snprintf(fname, 1024, "%s/data-rotate-test-manual-%05d.float", rootdir, i);
        if (i == 0 && stat(fname, &s) != 0) {
            printf("\tFAIL: %s not created\n", fname);
            goto do_return;
        } else if (i > 0 && stat(fname, &s) == 0) {
            printf("\tFAIL: %s created without rotate\n", fname);
            goto do_return;
        }
    }

    IO_HANDLE null = new_null_machine();
    add_write_filter(null, touch);

    const IOM *n = get_machine_ref(null);

    // Trigger file rotate
    size_t b1 = bytes;
    n->write(null, data, &b1);

    b1 = bytes;
    m->write(h1, data, &b1);

    char fname[1024];
    snprintf(fname, 1024, "%s/data-rotate-test-manual-%05d.float", rootdir, 1);

    struct stat s;
    if (stat(fname, &s) != 0) {
        printf("\tFAIL: %s not created\n", fname);
        goto do_return;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    return ret;
}

static int
run_dir_rotate_test(void *data, size_t bytes)
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    IO_HANDLE h0 = new_file_write_machine(rootdir, "test", "float");
    file_iom_set_auto_rotate(h0);
    IO_FILTER *touch0 = file_dir_rotate_filter(h0, NULL);

    IO_HANDLE h1 = new_file_write_machine(rootdir, "test", "float");
    file_iom_set_auto_rotate(h1);
    IO_FILTER *touch1 = file_dir_rotate_filter(h1, "dir-rotate-test");

    IO_HANDLE null = new_null_machine();
    add_write_filter(null, touch0);
    add_write_filter(null, touch1);
    const IOM *n = get_machine_ref(null);

    int i = 0;
    for (; i < 2; i++) {
        int j = 0;
        for (; j < 3; j++) {
            size_t b0, b1;
            b0 = b1 = bytes;
            m->write(h0, data, &b0);
            m->write(h1, data, &b1);

            char fname[1024];
            struct stat s;

            snprintf(fname, 1024, "%s/%05d/test-%05d.float", rootdir, i, j);
            if (stat(fname, &s) != 0) {
                printf("\tFAIL: %s not created\n", fname);
                goto do_return;
            }

            snprintf(fname, 1024, "%s/dir-rotate-test-%05d/test-%05d.float", rootdir, i, j);
            if (stat(fname, &s) != 0) {
                printf("\tFAIL: %s not created\n", fname);
                goto do_return;
            }
        }

        size_t b = bytes;
        n->write(null, data, &b);
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    return ret;
}

static int
run_auto_date_test(void *data, size_t bytes)
{
    // Set format to year/month
    // Write 3 files
    // verify indexing

    // set format to year/month/day
    // verify indexing

    int ret = 1;
    printf("%s\n", __FUNCTION__);

    IO_HANDLE h = new_file_write_machine(rootdir, "test", "float");
    file_iom_set_auto_rotate(h);
    file_iom_set_auto_date(h);
    file_iom_set_auto_date_fmt(h, "%Y/%m");
    IO_FILTER *touch = file_dir_rotate_filter(h, NULL);

    IO_HANDLE null = new_null_machine();
    add_write_filter(null, touch);
    const IOM *n = get_machine_ref(null);

    int i = 0;
    for (; i < 2; i++) {
        int j = 0;
        for (; j < 3; j++) {
            size_t b;
            b= bytes;
            m->write(h, data, &b);

            char fname[1024];
            struct stat s;

            char timestamp[32];
            gen_timestamp(timestamp, 32, "%Y/%m");
            snprintf(fname, 1024, "%s/%s/%05d/test-%05d.float",
                rootdir, timestamp, i, j);
            if (stat(fname, &s) != 0) {
                printf("\tFAIL: %s not created\n", fname);
                goto do_return;
            }
        }

        size_t b = bytes;
        n->write(null, data, &b);
    }

    file_iom_set_auto_date_fmt(h, "%Y/%m/%d");

    size_t b = bytes;
    m->write(h, data, &b);
    m->write(h, data, &b);
    m->write(h, data, &b);

    char fname[1024];
    char timestamp[32];
    gen_timestamp(timestamp, 32, "%Y/%m/%d");
    for (i = 0; i < 3; i++) {
        snprintf(fname, 1024, "%s/%s/%05d/test-%05d.float",
            rootdir, timestamp, 0, i);
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    return ret;
}

int
main(int nargs, char *argv[])
{
    fmt_rootdir("/tmp");
    printf("outdir: %s\n", rootdir);
    m = get_file_machine();
    float *data;
    size_t bytes = fill_float_data(100, &data);

    run_rw_test(data, bytes);
    run_read_test(data, bytes);
    run_write_test(data, bytes);
    run_rotate_test(data, bytes);
    run_dir_rotate_test(data, bytes);
    run_auto_date_test(data, bytes);

    if (data) {
        free(data);
    }

    if (rootdir) {
        free(rootdir);
    }
}
