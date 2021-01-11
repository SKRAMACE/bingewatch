#include <stdlib.h>

#include "machine.h"
#include "simple-buffers.h"
#include "test.h"
#include "logging.h"

#define LOGEX_TAG "RB-TEST"
#include <logex-main.h>

int
rw_test()
{
    int ret = 1;
 
    size_t wbytes = 1*MB;
    size_t rbytes = 2 * wbytes;
    float *buf = malloc(rbytes);

    float *data;
    fill_float_data(wbytes, &data);

    IO_HANDLE h = new_rb_machine();
    if (h == 0) {
        goto do_return;
    }

    size_t b = wbytes;
    rb_machine->write(h, data, &b);

    if (rb_get_bytes(h) != b) {
        goto do_return;
    }

    if (rb_get_bytes(h) != wbytes) {
        goto do_return;
    }

    b = rbytes;
    rb_machine->read(h, buf, &b);

    if (rb_get_bytes(h) != 0) {
        goto do_return;
    }

    if (b != wbytes) {
        goto do_return;
    }

    ret = 0;

do_return:
    rb_machine->stop(h);
    rb_machine->destroy(h);
    free(buf);
    return ret;
}

int
realloc_test()
{
    int ret = 1;
 
    size_t wbytes = 1*MB;
    char *data = malloc(wbytes);

    FILE *f = fopen("/dev/urandom", "r");

    char *p = data;
    size_t remaining = wbytes;
    while (remaining > 0) {
        size_t b = fread(p, 1, remaining, f);
        remaining -= b;
        p += b;
    }
    fclose(f);

    IO_HANDLE h = new_rb_machine();

    if (rb_get_size(h) != 0) {
        goto do_return;
    }

    size_t b = wbytes;
    rb_machine->write(h, data, &b);

    if (rb_get_size(h) % wbytes != 0) {
        goto do_return;
    }

    if (rb_get_bytes(h) != wbytes) {
        goto do_return;
    }

    remaining = rb_get_size(h) - 2*rb_get_bytes(h);
    while (remaining > 0) {
        size_t b = (remaining < wbytes) ? remaining : wbytes;
        rb_machine->write(h, data, &b);
        remaining -= b;
    }

    size_t s0 = rb_get_size(h);
    b = wbytes;
    rb_machine->write(h, data, &b);
    size_t s1 = rb_get_size(h);
    if (s0 == s1) {
        goto do_return;
    }

    ret = 0;

do_return:
    rb_machine->stop(h);
    rb_machine->destroy(h);
    free(data);
    return ret;
}


int
mem_limit_test()
{
    int ret = 1;
 
    size_t wbytes = 1*GB;
    char *data = malloc(wbytes);

    FILE *f = fopen("/dev/urandom", "r");

    char *p = data;
    size_t remaining = wbytes;
    while (remaining > 0) {
        size_t b = fread(p, 1, remaining, f);
        remaining -= b;
        p += b;
    }
    fclose(f);

    IO_HANDLE h = new_rb_machine();

    if (rb_get_size(h) != 0) {
        goto do_return;
    }

    size_t b = wbytes;
    rb_machine->write(h, data, &b);

    if (rb_get_size(h) % wbytes != 0) {
        goto do_return;
    }

    if (rb_get_bytes(h) != wbytes) {
        goto do_return;
    }

    remaining = rb_get_size(h) - 2*rb_get_bytes(h);
    while (remaining > 0) {
        size_t b = (remaining < wbytes) ? remaining : wbytes;
        rb_machine->write(h, data, &b);
        remaining -= b;
    }

    size_t s0 = rb_get_size(h);
    b = wbytes;
    rb_machine->write(h, data, &b);
    size_t s1 = rb_get_size(h);
    if (s0 != s1) {
        goto do_return;
    }

    ret = 0;

do_return:
    rb_machine->stop(h);
    rb_machine->destroy(h);
    free(data);
    return ret;
}

int
main(int nargs, char *argv[])
{
    test_init_logging(); 

    test_setup();

    test_add(rw_test);
    test_add(realloc_test);
    test_add(mem_limit_test);

    test_run();
    test_cleanup();
}
