#include "bw-uhd.h"
#include "sdrs.h"

#define LOGEX_TAG "BW-B210"
#include "logging.h"
#include "bw-log.h"

const IOM *b210_rx_machine;
static IOM *_b210_rx_machine = NULL;

void
b210_rx_set_gain(IO_HANDLE h, float lna)
{
    uhd_rx_set_gain(h, lna);
}

void
b210_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    uhd_set_rx(h, freq, rate, bandwidth);
}

int
b210_rx_set_freq(IO_HANDLE h, double freq)
{
    uhd_rx_set_freq(h, freq);
}

int
b210_rx_set_samp_rate(IO_HANDLE h, double samp_rate)
{
    uhd_rx_set_samp_rate(h, samp_rate);
}

int
b210_rx_set_bandwidth(IO_HANDLE h, double bandwidth)
{
    uhd_rx_set_bandwidth(h, bandwidth);
}

IO_HANDLE
new_b210_rx_machine_devstr(char *devstr)
{
    size_t ds_len = strlen(devstr) + 1;
    if (ds_len > (4096 - 16 - 1)) {
        error("Device string too long (%zd characters)", strlen(devstr));
        return 0;
    }

    char *ds = malloc(4096);
    snprintf(ds, 4095, "%s%s%s", "type=b200", (*devstr) ? "," : "", (*devstr) ?  devstr : "");
    ds[4095] = 0;

    IO_HANDLE h = new_uhd_rx_machine_devstr(0, ds);

    if (!_b210_rx_machine) {
        _b210_rx_machine = (IOM *)uhd_rx_machine;
        b210_rx_machine = _b210_rx_machine;
    }

    free(ds);
    return h;
}

IO_HANDLE
new_b210_rx_machine()
{
    return new_b210_rx_machine_devstr("");
}

void
b210_set_log_level(char *level)
{
    uhd_set_log_level(level);
    bw_set_log_level_str(level);
}
