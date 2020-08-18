#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "simple-machines.h"

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
run_udp_test(void *data, size_t bytes)
{
    int ret = 1;
    int ip = IP(127,0,0,1);

    IO_HANDLE dest = new_udp_server_machine(ip, 2222);
    IO_HANDLE src = new_udp_client_machine(ip, 2222);

    uint8_t *send = (uint8_t *)data;
    size_t b = bytes;
    socket_machine->write(src, send, &b);

    uint8_t *rcv = malloc(bytes);
    b = bytes;
    socket_machine->read(dest, rcv, &b);

    if (memcmp(send, rcv, bytes) != 0) {
        printf("\tFAIL: Data mismatch\n");
        goto do_return;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    if (rcv) {
        free(rcv);
    }

    return ret;
}

int
main(int nargs, char *argv[])
{
    float *data;
    size_t bytes = fill_float_data(100, &data);

    run_udp_test(data, bytes);

    if (data) {
        free(data);
    }
}
