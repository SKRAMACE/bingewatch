#include <stdio.h> //printf
#include <stdlib.h> //free
#include <string.h>
#include <complex.h>
#include <pthread.h>
#include <unistd.h>

#include "machine.h"
#include "filter.h"
#include "sdr-machine.h"
#include "sdrs.h"
#include "soapy.h"
#include "block-list-buffer.h"

#define LOGEX_TAG "BW-SOAPY"
#include "logging.h"
#include "bw-log.h"

#define SOAPY_SERIAL_LEN 32
#define MAX_ERROR_COUNT 0

enum soapy_var_e {
    SOAPY_VAR_FREQ,
    SOAPY_VAR_RATE,
    SOAPY_VAR_BANDWIDTH,
    SOAPY_VAR_PPM,
};

const IOM *soapy_rx_machine;
static IOM *_soapy_rx_machine = NULL;

// Machine Structs
struct soapy_api_t {
    SDR_API _sdr;
};

struct soapy_device_t {
    struct sdr_device_t _sdr;
};

static void
set_vars(POOL *p, void *args) {
}

// Cleanup Functions
static void
destroy_device(struct sdr_device_t *sdr)
{
    struct soapy_device_t *d = (struct soapy_device_t *)sdr;
    struct SoapySDRDevice *soapy = (struct SoapySDRDevice *)sdr->hw;
    SoapySDRDevice_unmake(soapy);
}

static void
destroy_soapy_channel(struct sdr_channel_t *chan)
{
    struct soapy_channel_t *c = (struct soapy_channel_t *)chan;
    SoapySDRDevice_deactivateStream(c->sdr, c->rx, 0, 0);
    SoapySDRDevice_closeStream(c->sdr, c->rx);
}

static int
soapy_channel_set(struct sdr_channel_t *sdr)
{
    struct soapy_channel_t *chan = (struct soapy_channel_t *)sdr;

    // Set antenna
    int ret = 0;
    switch(chan->antenna) {
    case LIME_MINI_LNAH:
        ret = SoapySDRDevice_setAntenna(chan->sdr, SOAPY_SDR_RX, 0, "LNAH");
        break;
    default:
        error("Unknown antenna \"%d\"", chan->antenna);
    }

    if (ret != 0) {
        error("setAntenna fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }
  
    // Set sample rate
    if (SoapySDRDevice_setSampleRate(chan->sdr, SOAPY_SDR_RX, 0, chan->_sdr.rate) != 0) {
        error("setSampleRate fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }
    chan->ns_per_sample = (double)1000000000 / (double)chan->_sdr.rate;

    // Set bandwidth
    double bandwidth = (chan->_sdr.bandwidth) ? chan->_sdr.bandwidth : chan->_sdr.rate;
    if (SoapySDRDevice_setBandwidth(chan->sdr, SOAPY_SDR_RX, 0, bandwidth) != 0) {
        error("setSampleRate fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Set frequency
    if (SoapySDRDevice_setFrequency(chan->sdr, SOAPY_SDR_RX, 0, chan->_sdr.freq, NULL) != 0) {
        error("setFrequency fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Set gain (LNA gain)
    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, "LNA", chan->_sdr.gain) != 0) {
        error("setGainElement fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Set other gain values
    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, "TIA", chan->tia_gain) != 0) {
        error("setGainElement fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, "PGA", chan->pga_gain) != 0) {
        error("setGainElement fail: %s", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    return IO_SUCCESS;
}

static int 
soapy_channel_reset(struct sdr_channel_t *sdr)
{
    struct soapy_channel_t *c = (struct soapy_channel_t *)sdr;
    if (SoapySDRDevice_deactivateStream(c->sdr, c->rx, 0, 0) != 0) {
        error("Soapy Error: %s\n", SoapySDRDevice_lastError());
        sdr->state = SDR_CHAN_ERROR;
        return IO_ERROR;
    }

    sdr->state = SDR_CHAN_RESET;
    return IO_SUCCESS;
}

static int
soapy_channel_start(struct sdr_channel_t *sdr)
{
    int ret = IO_ERROR;
    struct soapy_channel_t *chan = (struct soapy_channel_t *)sdr;

    trace("Starting channel %d:%d", chan->_sdr.device->id, chan->_sdr._d.handle);

    // Start Streaming
    SoapySDRDevice_activateStream(chan->sdr, chan->rx, 0, 0, 0);

    double ts = 0.01;
    size_t n_samp = (size_t)(chan->_sdr.rate * ts);
    char *data = malloc(n_samp * sizeof(float complex));
    void *buffs[] = {data};
    int flags;
    long long timeNs = 0;
    int diff = 0;

    double timeout = 2;

    // Read data until clock is synchronized
    while (timeNs == 0 || diff < -1 || diff > 1) {
        int samples_read = SoapySDRDevice_readStream(chan->sdr, chan->rx, buffs, n_samp, &flags, &timeNs, 100000);
        if (samples_read < 0) {
            int e = samples_read;
            const char *e_msg = SoapySDRDevice_lastError();
            error("SoapySDRDevice_readStream() error %d: %s", e, e_msg);
            return IO_ERROR;
        }

        long long expected = (long long)(chan->expected_timestamp + .5);
        int diff = (int)(timeNs - expected);
        chan->expected_timestamp = (double)timeNs + (double)samples_read * chan->ns_per_sample;

        timeout -= ts;
        if (timeout <= 0.0) {
            error("Channel %d:%d failed to synchronize timing", chan->_sdr.device->id, chan->_sdr._d.handle);
            goto do_return;
        }
    }
    ret = IO_SUCCESS;

do_return:
    free(data);
    return IO_SUCCESS;
}

// Read from device
static int
read_from_hw(struct sdr_channel_t *sdr, void *buf, size_t *n_samp)
{
    int ret = IO_ERROR;
    struct soapy_channel_t *chan = (struct soapy_channel_t *)sdr;

    float complex *data = (float complex *)buf;
    size_t remaining = *n_samp;
    size_t total_samp = 0;
    void *buffs[1];
    while (remaining) {
        *buffs = (void *)data;
        size_t n = remaining;
        int flags;
        long long timeNs;
        int samples_read;
        samples_read = SoapySDRDevice_readStream(chan->sdr, chan->rx, buffs, n, &flags, &timeNs, 100000);

        // Handle overflow errors
        if (samples_read < 0) {
            int e = samples_read;
            const char *e_msg = SoapySDRDevice_lastError();
            error("SoapySDRDevice_readStream() error %d: %s", e, e_msg);
            *n_samp = 0;
            goto do_return;
        }

        long long expected = (long long)(chan->expected_timestamp + .5);
        int diff = (int)(timeNs - expected);
        chan->expected_timestamp = (double)timeNs + (double)samples_read * chan->ns_per_sample;

        trace("expected: %zd, actual: %zd, diff: %d, samples: %d", expected, timeNs, diff, samples_read);
        if (diff < -1 || diff > 1) {
            if (!sdr->allow_overruns) {
                error("data read clock mismatch: %d: lost %zd samples",
                    diff, (size_t)((double)diff/chan->ns_per_sample));
                goto do_return;
            }

            debug("data read clock mismatch: %d: lost %zd samples",
                diff, (size_t)((double)diff/chan->ns_per_sample));

            ret = IO_DATABREAK;
            if (total_samp > 0) {
                goto do_return;
            }

        } else {
            chan->error_counter = 0;
        }

        data += samples_read;
        total_samp += samples_read;
        remaining -= (size_t)samples_read;
    }
    ret = (ret < IO_SUCCESS) ? IO_SUCCESS : ret;

do_return:
    *n_samp = total_samp;
    return ret;
}

// Device info
static void
enumerate_devices()
{
    size_t records;
    SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &records);

    printf("Found %d Soapy Devices", (int)records);
    if (records == 0) {
        return;
    }

    int i = 0;
    for (; i < records; i++) {
        printf("\t  %d: ", i);
        for (size_t j = 0; j < results[i].size; j++) {
            printf("%s=%s", results[i].keys[j], results[i].vals[j]);
            if (j < (results[i].size - 1)) {
                printf(", ");
            }
        }
        printf("\n");
    }
    SoapySDRKwargsList_clear(results, records);
}

static void
get_soapy_info(const char *str, int *id, char *serial)
{
    size_t records;
    SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &records);
    if (records == 0) {
        return;
    }

    *id = -1;
    serial[0] = '\0';

    int i = 0;
    for (; i < records; i++) {
        int j = 0;

        // Search for match in values
        for (; j < results[i].size; j++) {
            if (strcmp(str, results[i].vals[j]) == 0) {
                break;
            }
        }

        // Match not found
        if (j == results[i].size) {
            continue;
        }

        // Match found: Get serial
        *id = i;
        for (j = 0; j < results[i].size; j++) {
            if (strcmp("serial", results[i].keys[j]) == 0) {
                strncpy(serial, results[i].vals[j], SOAPY_SERIAL_LEN);
                SoapySDRKwargsList_clear(results, records);
                return;
            }
        }
    }
    SoapySDRKwargsList_clear(results, records);
}

static void
device_info(struct SoapySDRDevice *sdr)
{
    size_t length;

    //query device info
    printf("Rx antennas:\n");
    char** names = SoapySDRDevice_listAntennas(sdr, SOAPY_SDR_RX, 0, &length);
    size_t i = 0;
    for (; i < length; i++) {
        printf("\t%s\n", names[i]);
    }
    printf("\n");
    SoapySDRStrings_clear(&names, length);

    names = SoapySDRDevice_listGains(sdr, SOAPY_SDR_RX, 0, &length);
    printf("Rx gains:\n");
    for (i = 0; i < length; i++) {
        printf("\t%s\n", names[i]);
    }
    printf("\n");
    SoapySDRStrings_clear(&names, length);

    SoapySDRRange *ranges = SoapySDRDevice_getFrequencyRange(sdr, SOAPY_SDR_RX, 0, &length);
    printf("Rx freq ranges:\n");
    for (i = 0; i < length; i++) {
        printf("\t[%g Hz -> %g Hz]\n", ranges[i].minimum, ranges[i].maximum);
    }
    printf("\n");
    free(ranges);
}

/*  Example kwargs
 *      addr=24607:1027
 *      driver=lime
 *      label=LimeSDR Mini [USB 3.0] 1D4C463E946EFC
 *      media=USB 3.0
 *      module=FT601
 *      name=LimeSDR Mini
 *      serial=1D4C463E946EFC
 */
static struct sdr_device_t *
create_device(POOL *p, void *args)
{
    const char *id_str = (const char *)args;

    // Use id string to get device serial
    int id;
    char serial[SOAPY_SERIAL_LEN];
    get_soapy_info(id_str, &id, serial);
    if (id < 0) {
        error("Soapy Device \"%s\" Not Found", id_str);
        enumerate_devices();
        return NULL;
    }

    // Attempt to connect to device by serial
    SoapySDRKwargs kwargs = {};
    SoapySDRKwargs_set(&kwargs, "serial", serial);
    SoapySDRDevice *sdr = SoapySDRDevice_make(&kwargs);
    SoapySDRKwargs_clear(&kwargs);
    if (!sdr) {
        error("SoapySDRDevice_make fail: %s", SoapySDRDevice_lastError());
        return NULL;
    }

    // Success!  Now, create the IOM structs
    struct soapy_device_t *dev = pcalloc(p, sizeof(struct soapy_device_t));
    if (!dev) {
        error("Failed to allocate %zu bytes for sdr device\n", sizeof(struct soapy_device_t));
        free_pool(p);
        return 0;
    }

    // Initialize generic sdr device
    struct sdr_device_t *d = (struct sdr_device_t *)dev;
    pthread_mutex_init(&d->lock, NULL);
    d->id = id;
    d->hw = sdr;
    d->destroy_device_impl = destroy_device;

    return d;
}

static struct sdr_channel_t *
create_channel(POOL *p, struct sdr_device_t *dev, void *args)
{
    struct soapy_channel_t *chan = (struct soapy_channel_t *)pcalloc(p, sizeof(struct soapy_channel_t));

    chan->rx = SoapySDRDevice_setupStream(dev->hw, SOAPY_SDR_RX, SOAPY_SDR_CF32, &chan->chan, 1, NULL);
    if (!chan->rx)
    {
        error("setupStream fail: %s", SoapySDRDevice_lastError());
        return NULL;
    }

    chan->sdr = dev->hw;
    chan->_sdr.destroy_channel_impl = destroy_soapy_channel;

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
    fhw = create_filter(p, "_soapy_hw", sdrrx_read);
    fhw->obj = chan;

    if (!fhw) {
        error("Failed to create soapy rx filters");
        return NULL;
    }

    return fhw;
}

static void
api_init(IOM *machine)
{
    POOL *p = machine->alloc;
    struct soapy_api_t *api = (struct soapy_api_t *)pcalloc(p, sizeof(struct soapy_api_t));

    sdr_init_api_functions(machine, &api->_sdr);

    api->_sdr.set_vars = set_vars;
    api->_sdr.device = create_device;
    api->_sdr.channel = create_channel;
    api->_sdr.rx_filter = create_rx_filter;
    api->_sdr.tx_filter = NULL;

    if (ENVEX_EXISTS("BW_SOAPY_COUNTER_TEST")) {
        api->_sdr.hw_read = sdr_read_from_counter;
    } else {
        api->_sdr.hw_read = read_from_hw;
    }

    api->_sdr.channel_set = soapy_channel_set;
    api->_sdr.channel_reset = soapy_channel_reset;
    api->_sdr.channel_start = soapy_channel_start;
    
    machine->obj = api;
}

static IO_HANDLE
soapy_create(void *args)
{
    error("Implementation Error: Use \"sdr_create()\" to implement a soapy machine");
    return 0;
}

static int
soapy_set_gain_model(struct soapy_channel_t *soapy, GAIN_MODEL *model)
{
    int ret = 1;

    struct sdr_channel_t *chan = &soapy->_sdr;
    IO_DESC *d = (IO_DESC *)chan;

    while (d->in_use) {
        continue;
    }

    pthread_mutex_lock(&d->lock);
    if (SDR_CHAN_NOINIT == chan->state) {
        goto success;
    }

    SDR_API *api = (SDR_API *)_soapy_rx_machine->obj;
    api->set_gain_model(d->handle, model);

    SoapySDRDevice_deactivateStream(soapy->sdr, soapy->rx, 0, 0);

    if (soapy_channel_start(chan) < IO_SUCCESS) {
        goto soapy_error;
    }

success:
    pthread_mutex_unlock(&d->lock);
    return 0;

soapy_error:
    error("Soapy Error: %s\n", SoapySDRDevice_lastError());
    chan->state = SDR_CHAN_ERROR;

unlock_failure:
    pthread_mutex_unlock(&d->lock);
failure:
    return 1;
}


static int
soapy_set_val(IO_HANDLE h, int var, double val)
{
    int ret = 1;

    struct soapy_channel_t *soapy = soapy_get_channel(h);
    if (!soapy) {
        goto failure;
    }

    struct sdr_channel_t *chan = &soapy->_sdr;
    IO_DESC *d = (IO_DESC *)chan;

    while (d->in_use) {
        continue;
    }

    pthread_mutex_lock(&d->lock);
    switch (var) {
    case SOAPY_VAR_FREQ:
        chan->freq = val; break;
    case SOAPY_VAR_RATE:
        chan->rate = val; break;
    case SOAPY_VAR_BANDWIDTH:
        chan->bandwidth = val; break;
    case SOAPY_VAR_PPM:
        chan->ppm = val; break;
    default:
        error("Unknown Var (%d)", var);
        goto unlock_failure;
    }

    if (SDR_CHAN_NOINIT == chan->state) {
        goto success;
    }

    SoapySDRDevice_deactivateStream(soapy->sdr, soapy->rx, 0, 0);

    switch (var) {
    case SOAPY_VAR_FREQ:
        if (SoapySDRDevice_setFrequency(soapy->sdr, SOAPY_SDR_RX, 0, val, NULL) != 0) {
            goto soapy_error;
        }
        break;
    case SOAPY_VAR_RATE:
        if (SoapySDRDevice_setSampleRate(soapy->sdr, SOAPY_SDR_RX, 0, val) != 0) {
            goto soapy_error;
        }
        break;
    case SOAPY_VAR_BANDWIDTH:
        if (SoapySDRDevice_setBandwidth(soapy->sdr, SOAPY_SDR_RX, 0, val) != 0) {
            goto soapy_error;
        }
        break;
    case SOAPY_VAR_PPM:
        if (SoapySDRDevice_setFrequencyCorrection(soapy->sdr, SOAPY_SDR_RX, 0, val) != 0) {
            goto soapy_error;
        }
        break;
    default:
        error("Unknown Var (%d)", var);
        goto unlock_failure;
    }

    if (soapy_channel_start(chan) < IO_SUCCESS) {
        goto soapy_error;
    }

success:
    pthread_mutex_unlock(&d->lock);
    return 0;

soapy_error:
    error("Soapy Error: %s\n", SoapySDRDevice_lastError());
    chan->state = SDR_CHAN_ERROR;

unlock_failure:
    pthread_mutex_unlock(&d->lock);
failure:
    return 1;
}

int
soapy_rx_set_freq(IO_HANDLE h, double freq)
{
    return soapy_set_val(h, SOAPY_VAR_FREQ, freq);
}

int
soapy_rx_set_samp_rate(IO_HANDLE h, double samp_rate)
{
    return soapy_set_val(h, SOAPY_VAR_RATE, samp_rate);
}

int
soapy_rx_set_bandwidth(IO_HANDLE h, double bandwidth)
{
    return soapy_set_val(h, SOAPY_VAR_BANDWIDTH, bandwidth);
}

int
soapy_rx_set_ppm(IO_HANDLE h, double ppm)
{
    return soapy_set_val(h, SOAPY_VAR_PPM, ppm);
}

int
soapy_rx_set_gain_model(IO_HANDLE h, GAIN_MODEL *model)
{
    struct soapy_channel_t *soapy = soapy_get_channel(h);
    if (!soapy) {
        return 1;
    }

    return soapy_set_gain_model(soapy, model);
}

static void
soapy_log_init()
{
    char default_lvl[64];
    ENVEX_COPY(default_lvl, 64, "BW_LOG_LEVEL", "error");

    char lvl[64];
    ENVEX_COPY(lvl, 64, "BW_LIME_LOG_LEVEL", default_lvl);
    lime_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_SOAPY_LOG_LEVEL", default_lvl);
    soapy_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_SDRRX_LOG_LEVEL", default_lvl);
    sdrrx_set_log_level(lvl);
}

IOM *
get_soapy_rx_machine()
{
    IOM *machine = _soapy_rx_machine;
    if (!machine) {
        machine = machine_register("soapy_rx");

        sdr_init_machine_functions(machine);
        api_init(machine);

        // Local Functions
        machine->create = soapy_create;

        _soapy_rx_machine = machine;
        soapy_rx_machine = machine;

        soapy_log_init();
    }
    return machine;
}

IO_HANDLE
new_soapy_rx_machine(const char *id)
{
    IOM *machine = get_soapy_rx_machine();

    return sdr_create(machine, (void *)id);
}

struct soapy_channel_t *
soapy_get_channel(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Soapy channel %d not found", h);
        return NULL;
    }

    return (struct soapy_channel_t *)d;
}

int
soapy_rx_set_gain_elem(IO_HANDLE h, const char *elem, float gain)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    if (!chan) {
        goto failure;
    }

    // Set gain (LNA gain)
    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, elem, gain) != 0) {
        error("setGainElement fail: %s", SoapySDRDevice_lastError());
        goto failure;
    }

    return IO_SUCCESS;

failure:
    return IO_ERROR;
}

int
soapy_rx_get_gain_elem(IO_HANDLE h, const char *elem, float *gain)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    if (!chan) {
        return IO_ERROR;
    }

    // TODO: HOW TO DETECT AN ERROR
    *gain = (float)SoapySDRDevice_getGainElement(chan->sdr, SOAPY_SDR_RX, 0, elem);
    return IO_SUCCESS;
}

void
soapy_gain_elem_info(IO_HANDLE h, const char *elem)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    if (chan) {
        SoapySDRRange r = SoapySDRDevice_getGainElementRange(chan->sdr, SOAPY_SDR_RX, 0, elem);
        info("%s Gain Range: min=%0.2f, max=%0.2f, step=%0.2f", elem, r.minimum, r.maximum, r.step);
    }
}

void
soapy_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    if (!chan) {
        return;
    }

    chan->_sdr.freq = freq;
    chan->_sdr.rate = rate;
    chan->_sdr.bandwidth = bandwidth;
}

void
soapy_set_log_level(char *level)
{
    sdrrx_set_log_level(level);
    bw_set_log_level_str(level);
}
