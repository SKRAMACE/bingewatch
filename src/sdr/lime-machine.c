#include "soapy.h"
#include "sdrs.h"

#define LOGEX_TAG "BW-LIME"
#include "logging.h"
#include "bw-log.h"

#define ASSERT_LIME_MODEL(m,r) \
    if (m->type != GM_TYPE_LIME) {\
        error("Incompatible gain model type: expected %d, got %d", GM_TYPE_LIME, m->type); \
        return r; }

const IOM *lime_rx_machine;
static IOM *_lime_rx_machine = NULL;

struct lime_gain_model_t {
    struct bw_gain_model_t _gm;
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

int
lime_rx_get_net_gain(IO_HANDLE h, float *gain)
{
    float lna, pga, tia;
    if (soapy_rx_get_gain_elem(h, "LNA", &lna) != 0) {
        lna = 0;
        error("Failed to get LNA gain");
        return 1;
    }

    if (soapy_rx_get_gain_elem(h, "PGA", &pga) != 0) {
        pga = 0;
        error("Failed to get PGA gain");
        return 1;
    }

    if (soapy_rx_get_gain_elem(h, "TIA", &tia) != 0) {
        tia = 0;
        error("Failed to get TIA gain");
        return 1;
    }

    *gain = lna + pga + tia;
    return 0;
}

int
lime_rx_gain_inc(GAIN_MODEL *model)
{
    ASSERT_LIME_MODEL(model,-1);
    struct lime_gain_model_t *m = (struct lime_gain_model_t *)model;

    int max = 0;
    float inc = 3.0;

    float lna = 30.0 - m->lna;
    if (lna >= inc) {
        m->lna += inc;
        goto do_return;
    }
    m->lna += lna;
    inc -= lna;

    float pga = 19.0 - m->pga;
    if (pga >= inc) {
        m->pga += inc;
        goto do_return;
    }
    m->pga += pga;
    max = 1;

do_return:
    return max;
}

int
lime_rx_gain_dec(GAIN_MODEL *model)
{
    ASSERT_LIME_MODEL(model,-1);
    struct lime_gain_model_t *m = (struct lime_gain_model_t *)model;

    int min = 0;
    float dec = 3.0;

    if (m->pga >= dec) {
        m->pga -= dec;
        goto do_return;
    }
    dec -= m->pga;
    m->pga = 0;

    if (m->lna >= dec) {
        m->lna -= dec;
        goto do_return;
    }
    m->lna = 0;
    min = 1;

do_return:
    return min;
}

static void
api_init(IOM *machine)
{
    SDR_API *api = (SDR_API *)machine->obj;

    api->init_gain_model = lime_rx_gain_model_init;
    api->set_gain_model = lime_rx_set_gain_model;
    api->get_gain_model = lime_rx_get_gain_model;
    api->get_net_gain = lime_rx_get_net_gain;
    api->gain_inc = lime_rx_gain_inc;
    api->gain_dec = lime_rx_gain_dec;
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
lime_rx_set_gain_model(IO_HANDLE h, GAIN_MODEL *model)
{
    ASSERT_LIME_MODEL(model,1);
    struct lime_gain_model_t *m = (struct lime_gain_model_t *)model;

    set_gain_model_internal(h, m);
    return 0;
}

GAIN_MODEL *
lime_rx_gain_model_init(POOL *pool)
{
    GAIN_MODEL *model = pcalloc(pool, sizeof(struct lime_gain_model_t));
    model->type = GM_TYPE_LIME;
    model->len = sizeof(struct lime_gain_model_t);
    return model;
}

int
lime_rx_get_gain_model(IO_HANDLE h, GAIN_MODEL *model)
{
    ASSERT_LIME_MODEL(model,1);
    struct lime_gain_model_t *m = (struct lime_gain_model_t *)model;

    if (get_gain_model_internal(h, m) != 0) {
        return 1;
    }
    return 0;
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
