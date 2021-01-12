#include "soapy.h"
#include "sdrs.h"

#define LOGEX_TAG "BW-LIME"
#include "logging.h"
#include "bw-log.h"

const IOM *lime_rx_machine;
static IOM *_lime_rx_machine = NULL;

void
lime_set_gains(IO_HANDLE h, float lna, float tia, float pga)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    if (!chan) {
        return;
    }

    chan->_sdr.gain = lna;
    chan->tia_gain = tia;
    chan->pga_gain = pga;
}

void
lime_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    soapy_set_rx(h, freq, rate, bandwidth);
}

int
lime_rx_set_freq(IO_HANDLE h, double freq)
{
    soapy_rx_set_freq(h, freq);
}

int
lime_rx_set_samp_rate(IO_HANDLE h, double samp_rate)
{
    soapy_rx_set_samp_rate(h, samp_rate);
}

int
lime_rx_set_bandwidth(IO_HANDLE h, double bandwidth)
{
    soapy_rx_set_bandwidth(h, bandwidth);
}

IO_HANDLE
new_lime_rx_machine()
{
    IO_HANDLE h = new_soapy_rx_machine("lime");

    if (!_lime_rx_machine) {
        _lime_rx_machine = (IOM *)soapy_rx_machine;
        lime_rx_machine = _lime_rx_machine;
    }

    return h;
}

void
lime_set_log_level(char *level)
{
    soapy_set_log_level(level);
    bw_set_log_level_str(level);
}
