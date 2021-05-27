#include <stdio.h> //printf
#include <stdlib.h> //free
#include <string.h>
#include <complex.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>

#include "machine.h"
#include "filter.h"
#include "sdr-machine.h"
#include "sdrs.h"
#include "rtlsdr.h"
#include "block-list-buffer.h"

#define LOGEX_TAG "BW-RTLSDR"
#include "logging.h"
#include "bw-log.h"

#define RTLSDR_GAIN_MODE_AUTO 0
#define RTLSDR_GAIN_MODE_MANUAL 1

enum rtlsdr_var_e {
    RTLSDR_VAR_FREQ,
    RTLSDR_VAR_RATE,
    RTLSDR_VAR_BANDWIDTH,
    RTLSDR_VAR_PPM,
};

const IOM *rtlsdr_rx_machine;
static IOM *_rtlsdr_rx_machine = NULL;

// Machine Structs
struct rtlsdr_api_t {
    SDR_API _sdr;
};

struct rtlsdr_device_t {
    struct sdr_device_t _sdr;
};

static void
set_vars(POOL *p, void *args) {
}

// Cleanup Functions
static void
destroy_device(struct sdr_device_t *sdr)
{
    struct rtlsdr_device_t *d = (struct rtlsdr_device_t *)sdr;
    rtlsdr_dev_t *rtlsdr = (rtlsdr_dev_t *)sdr->hw;
    rtlsdr_close(rtlsdr);
}

static int
select_rtl_gain(struct rtlsdr_channel_t *chan)
{
    int *gains = chan->gain_vals;

    float g_target = chan->_sdr.gain;
    int g_ind = 0;
    float g_diff = 999999;
    for (int i = 0; i < chan->n_gain_vals; i++) {
        float g_cand = (float)gains[i] / 10.0;

        if (gains[i] == (int)(g_target * 10)) {
            return gains[i];
        }

        float diff = fabsf(g_target - g_cand);
        if (diff < g_diff) {
            g_diff = diff;
            g_ind = i;
        }
    }

    warn("rtlsdr gain \"%.1f\" is not supported.  Using \"%.1f\" instead.",
        g_target, (float)gains[g_ind] * 10);

    return gains[g_ind];
}

static int
rtlsdr_channel_set(struct sdr_channel_t *sdr)
{
    struct rtlsdr_channel_t *chan = (struct rtlsdr_channel_t *)sdr;

    rtlsdr_dev_t *rtl = (rtlsdr_dev_t *)chan->sdr;

    // Set frequency
    if (rtlsdr_set_center_freq(rtl, (uint32_t)chan->_sdr.freq) != 0) {
        error("set_center_freq() fail");
        return IO_ERROR;
    }

    // Set sample rate
    if (rtlsdr_set_sample_rate(rtl, (uint32_t)chan->_sdr.rate) != 0) {
        error("set_sample_rate() fail");
        return IO_ERROR;
    }

    // Set bandwidth
    double bandwidth = (chan->_sdr.bandwidth) ? chan->_sdr.bandwidth : chan->_sdr.rate;
    if (rtlsdr_set_tuner_bandwidth(rtl, (uint32_t)bandwidth) != 0) {
        error("set_tuner_bandwidth() fail");
        return IO_ERROR;
    }

    // Gain mode
    if (rtlsdr_set_tuner_gain_mode(rtl, chan->gain_mode) != 0) {
        error("set_tuner_gain_mode() fail");
        return IO_ERROR;
    }

    // Set gain (LNA gain)
    if (RTLSDR_GAIN_MODE_MANUAL == chan->gain_mode) {
        int gain = select_rtl_gain(chan);
        if (rtlsdr_set_tuner_gain(rtl, gain) != 0) {
            error("set_tuner_gain() fail");
            return IO_ERROR;
        }
    }

    if (rtlsdr_set_testmode(rtl, chan->test_mode) != 0) {
        error("Failed to set test mode");
        return IO_ERROR;
    }

    return IO_SUCCESS;
}

static int 
rtlsdr_channel_reset(struct sdr_channel_t *sdr)
{
    sdr->state = SDR_CHAN_RESET;
    return IO_SUCCESS;
}

static int
rtlsdr_channel_start(struct sdr_channel_t *sdr)
{
    int ret = IO_ERROR;
    struct rtlsdr_channel_t *chan = (struct rtlsdr_channel_t *)sdr;
    rtlsdr_dev_t *rtl = (rtlsdr_dev_t *)chan->sdr;

    trace("Starting channel %d:%d", chan->_sdr.device->id, chan->_sdr._d.handle);

    // Start Streaming
    if (rtlsdr_reset_buffer(rtl) != 0) {
        error("reset_buffer() fail");
        return IO_ERROR;
    }

    double ts = 0.01;
    size_t n_samp = (size_t)(chan->_sdr.rate * ts);
    size_t n_bytes = n_samp * sizeof(uint16_t);
    uint8_t *data = malloc(n_bytes);

    double timeout = 2;
    while (1) {
        int blen = (int)n_bytes;
        int bytes;
        if (rtlsdr_read_sync(rtl, data, blen, &bytes) != 0) {
            goto next;
        }

        if (bytes != blen) {
            goto next;
        }

        break;
        
    next:
        timeout -= ts;
        if (timeout <= 0.0) {
            error("Channel %d:%d failed to synchronize timing", chan->_sdr.device->id, chan->_sdr._d.handle);
            goto do_return;
        }
    }
    ret = IO_SUCCESS;

do_return:
    free(data);
    return ret;
}

static void
uint8_to_float(float complex *fc, uint16_t *uc, size_t n_samp) {
    float *f = (float *)fc;
    uint8_t *u = (uint8_t *)uc;

    for (size_t i = 0; i < n_samp * 2; i++) {
        f[i] = ((float)u[i] - 127.5) / 127.5;
    }
}

// Read from device
static int
read_from_hw(struct sdr_channel_t *sdr, void *buf, size_t *n_samp)
{
    int ret = IO_ERROR;
    struct rtlsdr_channel_t *chan = (struct rtlsdr_channel_t *)sdr;
    rtlsdr_dev_t *rtl = (rtlsdr_dev_t *)chan->sdr;

    // output expected in float complex samples, but the rtl-sdr reads uint8 complex samples
    // Align the read buffer to the end of the output buffer
    size_t n = *n_samp;
    float complex *data_fc = (float complex *)buf;
    uint16_t *data_uc = (uint16_t *)(data_fc + n) - n;

    // Read samples
    uint8_t *rb = (uint8_t *)data_uc;
    size_t remaining = n;
    size_t total_samp = 0;
    while (remaining) {
        int blen = (int)(remaining * sizeof(uint16_t));
        int bytes;
        if (rtlsdr_read_sync(rtl, rb, blen, &bytes) != 0) {
            error("read_sync() error");
            *n_samp = 0;
            goto do_return;
        }

        int samples_read = bytes / sizeof(uint16_t);

        rb += samples_read;
        total_samp += samples_read;
        remaining -= (size_t)samples_read;
    }

    uint8_to_float(data_fc, data_uc, total_samp);

    ret = (ret < IO_SUCCESS) ? IO_SUCCESS : ret;

do_return:
    *n_samp = total_samp;
    return ret;
}

// Device info
static void
enumerate_devices()
{
	int n_dev = rtlsdr_get_device_count();

    char vendor[256], product[256], serial[256];
    info("RTLSDR Devices");
    for (int i = 0; i < n_dev; i++) {
        char vendor[256], product[256], serial[256];
        rtlsdr_get_device_usb_strings(i, vendor, product, serial);
        info("[%d] vendor=%s, product=%s, serial=%s",
            i, vendor, product, serial);
    }
}

static struct sdr_device_t *
create_device(POOL *p, void *args)
{
	int n_dev = rtlsdr_get_device_count();
    if (n_dev == 0) {
        error("No RTLSDR devices found");
        return NULL;
    }

    // Default to first device
    int index = -1;

    // If a device string is given, try to find it
    const char *id_str = (const char *)args;
    if (id_str) {
        char vendor[256], product[256], serial[256];
        for (int i = 0; i < n_dev; i++) {
            rtlsdr_get_device_usb_strings(i, vendor, product, serial);
            if (atoi(id_str) == i) {
                index = i;
                break;
            }

            if (strncmp(id_str, serial, 256) == 0) {
                index = i;
                break;
            }
        }
    } else {
        index = 0;
    }

    if (index < 0) {
        error("RTLSDR device \"%s\" Not Found", id_str);
        enumerate_devices();
        return NULL;
    }

    rtlsdr_dev_t *sdr;
	if (rtlsdr_open(&sdr, (uint32_t)index) != 0) {
		error("Failed to open rtlsdr device %d", index);
        return NULL;
	}

    // Success!  Now, create the IOM structs
    struct rtlsdr_device_t *dev = pcalloc(p, sizeof(struct rtlsdr_device_t));

    // Initialize generic sdr device
    struct sdr_device_t *d = (struct sdr_device_t *)dev;
    pthread_mutex_init(&d->lock, NULL);
    d->id = index;
    d->hw = sdr;
    d->destroy_device_impl = destroy_device;

    return d;
}

static struct sdr_channel_t *
create_channel(POOL *p, struct sdr_device_t *dev, void *args)
{
    struct rtlsdr_channel_t *chan = (struct rtlsdr_channel_t *)pcalloc(p, sizeof(struct rtlsdr_channel_t));
    chan->sdr = dev->hw;
    rtlsdr_dev_t *rtl = (rtlsdr_dev_t *)chan->sdr;

    chan->gain_mode = RTLSDR_GAIN_MODE_MANUAL;
    if (rtlsdr_set_tuner_gain_mode(rtl, chan->gain_mode) != 0) {
        error("Failed to set gain mode");
        return NULL;
    }

    int n_val = rtlsdr_get_tuner_gains(rtl, NULL);
    if (n_val < 0) {
        error("Failed to get device info");
        return NULL;
    }

    int *gains = palloc(p, n_val * sizeof(int));
    rtlsdr_get_tuner_gains(rtl, gains);

    chan->n_gain_vals = n_val;
    chan->gain_vals = gains;

    return (struct sdr_channel_t *)chan;
}

static struct io_filter_t *
create_rx_filter(POOL *p, struct sdr_channel_t *chan, struct sdr_device_t *dev)
{
    // Hardware filter returns optimized number of samples when available
    struct io_filter_t *fhw;

    // TODO: Create a data filter to guarantee the correct number of bytes
    struct io_filter_t *fdata;

    // Create hardware filter, and set channel descriptor as the filter object
    fhw = create_filter(p, "_rtlsdr_hw", sdrrx_read);
    fhw->obj = chan;

    if (!fhw) {
        error("Failed to create rtlsdr rx filters");
        return NULL;
    }

    return fhw;
}

static void
api_init(IOM *machine)
{
    POOL *p = machine->alloc;
    struct rtlsdr_api_t *api = (struct rtlsdr_api_t *)pcalloc(p, sizeof(struct rtlsdr_api_t));

    sdr_init_api_functions(machine, &api->_sdr);

    api->_sdr.set_vars = set_vars;
    api->_sdr.device = create_device;
    api->_sdr.channel = create_channel;
    api->_sdr.rx_filter = create_rx_filter;
    api->_sdr.tx_filter = NULL;

    api->_sdr.hw_read = read_from_hw;

    api->_sdr.channel_set = rtlsdr_channel_set;
    api->_sdr.channel_reset = rtlsdr_channel_reset;
    api->_sdr.channel_start = rtlsdr_channel_start;
    
    machine->obj = api;
}

static IO_HANDLE
rtlsdr_create(void *args)
{
    error("Implementation Error: Use \"sdr_create()\" to implement an rtlsdr machine");
    return 0;
}

static int
rtlsdr_set_val(IO_HANDLE h, int var, double val)
{
    int ret = 1;

    struct rtlsdr_channel_t *rtlsdr = rtlsdr_get_channel(h);
    if (!rtlsdr) {
        goto failure;
    }

    struct sdr_channel_t *chan = &rtlsdr->_sdr;
    IO_DESC *d = (IO_DESC *)chan;

    while (d->in_use) {
        continue;
    }

    pthread_mutex_lock(&d->lock);
    switch (var) {
    case RTLSDR_VAR_FREQ:
        chan->freq = val; break;
    case RTLSDR_VAR_RATE:
        chan->rate = val; break;
    case RTLSDR_VAR_BANDWIDTH:
        chan->bandwidth = val; break;
    case RTLSDR_VAR_PPM:
        chan->ppm = val; break;
    default:
        error("Unknown Var (%d)", var);
        goto unlock_failure;
    }

    if (SDR_CHAN_NOINIT == chan->state) {
        goto success;
    }

    switch (var) {
    case RTLSDR_VAR_FREQ:
        if (rtlsdr_set_center_freq(rtlsdr->sdr, (uint32_t)val) != 0) {
            goto rtlsdr_error;
        }
        break;
    case RTLSDR_VAR_RATE:
        if (rtlsdr_set_sample_rate(rtlsdr->sdr, (uint32_t)val) != 0) {
            goto rtlsdr_error;
        }
        break;
    case RTLSDR_VAR_BANDWIDTH:
        if (rtlsdr_set_tuner_bandwidth(rtlsdr->sdr, (uint32_t)val) != 0) {
            goto rtlsdr_error;
        }
        break;
    case RTLSDR_VAR_PPM:
        if (rtlsdr_set_freq_correction(rtlsdr->sdr, (int)val) != 0) {
            goto rtlsdr_error;
        }
        break;
    default:
        error("Unknown Var (%d)", var);
        goto unlock_failure;
    }

    if (rtlsdr_channel_start(chan) < IO_SUCCESS) {
        goto rtlsdr_error;
    }

success:
    pthread_mutex_unlock(&d->lock);
    return 0;

rtlsdr_error:
    error("RTL-SDR Error");
    chan->state = SDR_CHAN_ERROR;

unlock_failure:
    pthread_mutex_unlock(&d->lock);
failure:
    return 1;
}

int
rtlsdr_rx_set_freq(IO_HANDLE h, double freq)
{
    return rtlsdr_set_val(h, RTLSDR_VAR_FREQ, freq);
}

int
rtlsdr_rx_set_samp_rate(IO_HANDLE h, double samp_rate)
{
    return rtlsdr_set_val(h, RTLSDR_VAR_RATE, samp_rate);
}

int
rtlsdr_rx_set_bandwidth(IO_HANDLE h, double bandwidth)
{
    return rtlsdr_set_val(h, RTLSDR_VAR_BANDWIDTH, bandwidth);
}

int
rtlsdr_rx_set_ppm(IO_HANDLE h, double ppm)
{
    return rtlsdr_set_val(h, RTLSDR_VAR_PPM, ppm);
}

int
rtlsdr_rx_set_testmode(IO_HANDLE h)
{
    struct rtlsdr_channel_t *rtlsdr = rtlsdr_get_channel(h);
    rtlsdr->test_mode = 1;
    return IO_SUCCESS;
}

static void
rtlsdr_log_init()
{
    char default_lvl[64];
    ENVEX_COPY(default_lvl, 64, "BW_LOG_LEVEL", "error");

    char lvl[64];
    ENVEX_COPY(lvl, 64, "BW_RTLSDR_LOG_LEVEL", default_lvl);
    rtlsdr_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_SDRRX_LOG_LEVEL", default_lvl);
    sdrrx_set_log_level(lvl);
}

IOM *
get_rtlsdr_rx_machine()
{
    IOM *machine = _rtlsdr_rx_machine;
    if (!machine) {
        machine = machine_register("rtlsdr_rx");

        sdr_init_machine_functions(machine);
        api_init(machine);

        // Local Functions
        machine->create = rtlsdr_create;

        _rtlsdr_rx_machine = machine;
        rtlsdr_rx_machine = machine;

        rtlsdr_log_init();
    }
    return machine;
}

IO_HANDLE
new_rtlsdr_rx_machine(const char *id)
{
    IOM *machine = get_rtlsdr_rx_machine();

    return sdr_create(machine, (void *)id);
}

struct rtlsdr_channel_t *
rtlsdr_get_channel(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("RTL-SDR channel %d not found", h);
        return NULL;
    }

    return (struct rtlsdr_channel_t *)d;
}

void
rtlsdr_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    struct rtlsdr_channel_t *chan = rtlsdr_get_channel(h);
    if (!chan) {
        return;
    }

    chan->_sdr.freq = freq;
    chan->_sdr.rate = rate;
    chan->_sdr.bandwidth = bandwidth;
}

void
rtlsdr_rx_set_gain(IO_HANDLE h, float lna)
{
    struct rtlsdr_channel_t *chan = rtlsdr_get_channel(h);
    if (!chan) {
        return;
    }
    chan->_sdr.gain = lna;
}

void
rtlsdr_set_log_level(char *level)
{
    sdrrx_set_log_level(level);
    bw_set_log_level_str(level);
}
