#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "machine.h"
#include "simple-buffers.h"
#include "test.h"

#define LOGEX_TAG "BUF-TEST"
#include <logex-main.h>

static void
print_buf(char *buf, size_t bytes) {
    size_t i = 0;

    if (0 == bytes) {
        printf("0 bytes\n");
        return;
    }

    for(; i < bytes; i++) {
        printf("%c", buf[i]);
    }

    printf("\n");
}

static int
rb_write()
{
    return 1;
}

static int
rb_min_return_size()
{
    return 1;
}

int
fbb()
{
    int ret = 1;

    IO_HANDLE h = new_fbb_machine(2, 10);

    char buf[20];
    size_t bytes;

    memset(buf, 'A', 20);
    bytes = 20;
    fbb_machine->write(h, buf, &bytes);

    memset(buf, 'A', 20);
    bytes = 10;
    fbb_machine->write(h, buf, &bytes);

    memset(buf, 'B', 20);
    bytes = 5;
    fbb_machine->write(h, buf, &bytes);

    memset(buf, 'C', 20);
    bytes = 1;
    fbb_machine->write(h, buf, &bytes);

    bytes = 1;
    fbb_machine->read(h, buf, &bytes);
    print_buf(buf, bytes);

    int i = 0;
    for (; i < 4; i++) {
        bytes = 20;
        fbb_machine->read(h, buf, &bytes);
        print_buf(buf, bytes);
    }

do_return:
    fbb_machine->destroy(h);
    return ret;
}

int
main(int nargs, char *argv[])
{
    test_init_logging(); 

    test_setup();

    test_add(rb_write);
    test_add(rb_min_return_size);
    test_add(fbb);

    test_run();
    test_cleanup();
}
