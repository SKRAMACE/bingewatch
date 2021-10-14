#include "soapy.h"
#include "sdrs.h"

#define LOGEX_TAG "BW-LIME"
#include "logging.h"
#include "bw-log.h"

const IOM *lime_rx_machine;
static IOM *_lime_rx_machine = NULL;

struct lime_gain_model_t {
    float lna;
    float pga;
    float tia;
};

static void
set_gain_model_internal(IO_HANDLE h, struct lime_gain_model_t *model)
{
    if (soapy_rx_set_gain_elem(h, "LNA", model->lna) != 0) {
        return;
    }

    if (soapy_rx_set_gain_elem(h, "PGA", model->pga) != 0) {
        return;
    }

    if (soapy_rx_set_gain_elem(h, "TIA", model->tia) != 0) {
        return;
    }
}

static void
set_gain_model(IO_HANDLE h, BW_GAIN model)
{
    struct lime_gain_model_t *m = (struct lime_gain_model_t *)model;
    set_gain_model_internal(h, m);
}

static int
get_gain_model_internal(IO_HANDLE h, struct lime_gain_model_t *model)
{
    float lna, pga, tia;
    if (soapy_rx_get_gain_elem(h, "LNA", &lna) != 0) {
        goto failure;
    }

    if (soapy_rx_get_gain_elem(h, "PGA", &pga) != 0) {
        goto failure;
    }

    if (soapy_rx_get_gain_elem(h, "TIA", &tia) != 0) {
        goto failure;
    }

    model->lna = lna;
    model->pga = pga;
    model->tia = tia;
    return 0;

failure:
    return 1;
}

static BW_GAIN
get_gain_model(IO_HANDLE h, POOL *pool)
{
    struct lime_gain_model_t m;
    if (get_gain_model_internal(h, &m) != 0) {
        return NULL;
    }

    struct lime_gain_model_t *model = pcalloc(pool, sizeof(struct lime_gain_model_t));
    model->lna = m.lna;
    model->pga = m.pga;
    model->tia = m.tia;

    return (BW_GAIN)model;
}

static float
get_net_gain(IO_HANDLE h)
{
    float lna, pga, tia;
    if (soapy_rx_get_gain_elem(h, "LNA", &lna) != 0) {
        lna = 0;
        error("Failed to get LNA gain");
    }

    if (soapy_rx_get_gain_elem(h, "PGA", &pga) != 0) {
        pga = 0;
        error("Failed to get PGA gain");
    }

    if (soapy_rx_get_gain_elem(h, "TIA", &tia) != 0) {
        tia = 0;
        error("Failed to get TIA gain");
    }

    return lna + pga + tia;
}

int
lime_rx_get_net_gain(IO_HANDLE h, float *gain)
{
    *gain = get_net_gain(h);
}

static int
gain_inc(IO_HANDLE h)
{
    int max = 0;

    struct lime_gain_model_t m;
    get_gain_model_internal(h, &m);

    float inc = 3.0;

    float lna = 30.0 - m.lna;
    if (lna >= inc) {
        m.lna += inc;
        goto set_gain;
    }
    m.lna += lna;
    inc -= lna;

    float pga = 19.0 - m.pga;
    if (pga >= inc) {
        m.pga += inc;
        goto set_gain;
    }
    m.pga += pga;
    max = 1;

set_gain:
    set_gain_model_internal(h, &m);
    return max;
}

int
lime_rx_gain_inc(IO_HANDLE h)
{
    return gain_inc(h);
}

static int
gain_dec(IO_HANDLE h)
{
    int min = 0;

    struct lime_gain_model_t m;
    get_gain_model_internal(h, &m);

    float dec = 3.0;

    if (m.pga >= dec) {
        m.pga -= dec;
        goto set_gain;
    }
    dec -= m.pga;
    m.pga = 0;

    if (m.lna >= dec) {
        m.lna -= dec;
        goto set_gain;
    }
    m.lna = 0;
    min = 1;

set_gain:
    set_gain_model_internal(h, &m);
    return min;
}

int
lime_rx_gain_dec(IO_HANDLE h)
{
    return gain_dec(h);
}

static void
api_init(IOM *machine)
{
    SDR_API *api = (SDR_API *)machine->obj;

    api->set_gain_model = set_gain_model;
    api->get_gain_model = get_gain_model;
    api->get_net_gain = get_net_gain;
    api->gain_inc = gain_inc;
    api->gain_dec = gain_dec;
}

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
lime_gain_info(IO_HANDLE h)
{
    soapy_gain_elem_info(h, "LNA");
    soapy_gain_elem_info(h, "PGA");
    soapy_gain_elem_info(h, "TIA");
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

int
lime_rx_set_ppm(IO_HANDLE h, double ppm)
{
    return soapy_rx_set_ppm(h, ppm);
}

int
lime_rx_set_gain_model(IO_HANDLE h, BW_GAIN model)
{
    set_gain_model(h, model);
    return 0;
}

BW_GAIN
lime_rx_get_gain_model(IO_HANDLE h, POOL *pool)
{
    return get_gain_model(h, pool);
}

IO_HANDLE
new_lime_rx_machine()
{
    IO_HANDLE h = new_soapy_rx_machine("lime");

    if (!_lime_rx_machine) {
        _lime_rx_machine = (IOM *)soapy_rx_machine;
        api_init(_lime_rx_machine);
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
