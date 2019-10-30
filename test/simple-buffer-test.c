#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "machine.h"
#include "simple-buffers.h"

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

int
main(int argc, char *argv[]) {
    IO_HANDLE h;
    const IOM *fbb = new_fbb_machine(&h, 2, 10);

    char buf[20];
    size_t bytes;

    memset(buf, 'A', 20);
    bytes = 20;
    fbb->write(h, buf, &bytes);
    bytes = 10;
    fbb->write(h, buf, &bytes);

    memset(buf, 'B', 20);
    bytes = 5;
    fbb->write(h, buf, &bytes);

    memset(buf, 'C', 20);
    bytes = 1;
    fbb->write(h, buf, &bytes);

    bytes = 1;
    fbb->read(h, buf, &bytes);
    print_buf(buf, bytes);

    int i = 0;
    for (; i < 4; i++) {
        bytes = 20;
        fbb->read(h, buf, &bytes);
        print_buf(buf, bytes);
    }
}
