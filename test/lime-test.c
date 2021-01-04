#include <stdlib.h>
#include <stdio.h>
#include <uuid/uuid.h>

#include "sdrs.h"
#include "logging.h"

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

static int
run_read_test()
{
    int ret = 1;
    printf("%s\n", __FUNCTION__);

    double freq = 1000000000.0;
    double samp_rate = 30720000.0;
    double bandwidth = samp_rate;

    size_t bytes = (size_t)(samp_rate * .01);
    char *buf = malloc(bytes);

    IO_HANDLE lime = new_soapy_rx_machine("lime");
    soapy_set_rx(lime, freq, samp_rate, bandwidth);

    size_t remaining = (size_t)samp_rate;
    while (remaining) {
        size_t b = (bytes < remaining) ? bytes : remaining;
        if (soapy_rx_machine->read(lime, buf, &b) != IO_SUCCESS) {
            goto do_return;
        }
        remaining -= b;
    }

    ret = 0;
    printf("\tPASS\n");

do_return:
    soapy_rx_machine->stop(lime);
    soapy_rx_machine->destroy(lime);

    if (buf) {
        free(buf);
    }

    return ret;
}

int
main(int nargs, char *argv[])
{
    soapy_set_log_level("error"); 

    fmt_rootdir("/tmp");
    printf("outdir: %s\n", rootdir);

    run_read_test();
    run_read_test();

    if (rootdir) {
        free(rootdir);
    }
}
