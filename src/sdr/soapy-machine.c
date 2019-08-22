#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <stdio.h> //printf
#include <stdlib.h> //free
#include <string.h>
#include <complex.h>
#include <radpool.h>

#include "machine.h"
#include "filter.h"
#include "sdr-machine.h"
#include "sdrs.h"

#define SOAPY_SERIAL_LEN 32

// Machine Implementation List
static IOM *soapy_rx_machine = NULL;

// Machine Structs
struct soapy_api_t {
    SDR_API _sdr;
};

enum soapy_antennas_e {
    LIME_MINI_LNAH,
};

struct soapy_device_t {
    struct sdr_device_t _sdr;
};

struct soapy_channel_t {
    struct sdr_channel_t _sdr;
    SoapySDRDevice *sdr;
    SoapySDRStream *rx;
    enum soapy_antennas_e antenna;
    float tia_gain;
    float pga_gain;
};

static struct soapy_channel_t *
soapy_get_channel(IO_HANDLE h)
{
    const struct sdr_device_t *dev = sdr_get_device(h);
    struct sdr_channel_t *chan = sdr_get_channel(h, dev);
    return (struct soapy_channel_t *)chan;
}

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
soapy_channel_init(struct soapy_channel_t *chan)
{
    // Set antenna
    int ret = 0;
    switch(chan->antenna) {
    case LIME_MINI_LNAH:
        ret = SoapySDRDevice_setAntenna(chan->sdr, SOAPY_SDR_RX, 0, "LNAH");
        break;
    default:
        printf("ERROR: Unknown antenna \"%d\"\n", chan->antenna);
    }

    if (ret != 0) {
        printf("setAntenna fail: %s\n", SoapySDRDevice_lastError());
        return IO_ERROR;
    }
  
    // Set sample rate
    if (SoapySDRDevice_setSampleRate(chan->sdr, SOAPY_SDR_RX, 0, chan->_sdr.rate) != 0) {
        printf("setSampleRate fail: %s\n", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Set frequency
    if (SoapySDRDevice_setFrequency(chan->sdr, SOAPY_SDR_RX, 0, chan->_sdr.freq, NULL) != 0) {
        printf("setFrequency fail: %s\n", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Set gain (LNA gain)
    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, "LNA", chan->_sdr.gain) != 0) {
        printf("setGainElement fail: %s\n", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Set other gain values
    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, "TIA", chan->tia_gain) != 0) {
        printf("setGainElement fail: %s\n", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    if (SoapySDRDevice_setGainElement(chan->sdr, SOAPY_SDR_RX, 0, "PGA", chan->pga_gain) != 0) {
        printf("setGainElement fail: %s\n", SoapySDRDevice_lastError());
        return IO_ERROR;
    }

    // Start Streaming
    SoapySDRDevice_activateStream(chan->sdr, chan->rx, 0, 0, 0);

    return IO_SUCCESS;
}

// Read from device
static int
read_data_from_hw(IO_FILTER_ARGS)
{
    // Dereference channel from filter object
    struct soapy_channel_t *chan = (struct soapy_channel_t *)IO_FILTER_ARGS_FILTER->obj;

    if (!chan->_sdr.init) {
        if (soapy_channel_init(chan) != IO_SUCCESS) {
            printf("ERROR: Failed to init channel\n");
            return IO_ERROR;
        }
        chan->_sdr.init = 1;
    }

    void *buffs[] = {IO_FILTER_ARGS_BUF};
    int flags;
    long long timeNs;

    size_t bytes = *IO_FILTER_ARGS_BYTES / sizeof(complex float);
    int samples_read;
    samples_read = SoapySDRDevice_readStream(chan->sdr, chan->rx, buffs, bytes, &flags, &timeNs, 100000);
    if (samples_read < 0) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    *IO_FILTER_ARGS_BYTES = samples_read * sizeof(complex float);
    return IO_SUCCESS;
}

// Device info
static void
enumerate_devices()
{
    size_t records;
    SoapySDRKwargs *results = SoapySDRDevice_enumerate(NULL, &records);
    printf("\tFound 0 Soapy Devices\n");
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
        printf("ERROR: Soapy Device \"%s\" Not Found\n", id_str);
        enumerate_devices();
        return NULL;
    }

    // Attempt to connect to device by serial
    SoapySDRKwargs kwargs = {};
    SoapySDRKwargs_set(&kwargs, "serial", serial);
    SoapySDRDevice *sdr = SoapySDRDevice_make(&kwargs);
    SoapySDRKwargs_clear(&kwargs);
    if (!sdr) {
        printf("ERROR: SoapySDRDevice_make fail: %s\n", SoapySDRDevice_lastError());
        return NULL;
    }

    // Success!  Now, create the IOM structs
    struct soapy_device_t *dev = pcalloc(p, sizeof(struct soapy_device_t));
    if (!dev) {
        printf("ERROR: Failed to allocate %ld bytes for sdr device\n",
            sizeof(struct soapy_device_t));
        pfree(p);
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

    if (SoapySDRDevice_setupStream(dev->hw, &chan->rx, SOAPY_SDR_RX, SOAPY_SDR_CF32,
            NULL, 0, NULL) != 0)
    {
        printf("setupStream fail: %s\n", SoapySDRDevice_lastError());
        return NULL;
    }

    chan->sdr = dev->hw;
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
    fhw = create_filter(p, "_soapy_hw", read_data_from_hw);
    fhw->obj = chan;

    if (!fhw) {
        printf("Error creating rx filters\n");
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
    
    machine->obj = api;
}

static IO_HANDLE
soapy_create(void *args)
{
    printf("ERROR: Implementation Error: Use \"sdr_create()\" to implement a soapy machine\n");
    return 0;
}

const IOM *
get_soapy_rx_machine()
{
    IOM *machine = soapy_rx_machine;
    if (!machine) {
        machine = machine_register("soapy_rx_machine");

        sdr_init_machine_functions(machine);
        api_init(machine);

        // Local Functions
        machine->create = soapy_create;

        soapy_rx_machine = machine;
    }
    return (const IOM *)machine;
}

IO_HANDLE
new_soapy_rx_machine(const char *id)
{
    const IOM *machine = get_soapy_rx_machine();

    return sdr_create(machine, (void *)id);
}

void
soapy_set_gains(IO_HANDLE h, float lna, float tia, float pga)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    chan->_sdr.gain = lna;
    chan->tia_gain = tia;
    chan->pga_gain = pga;
}

void
soapy_set_rx(IO_HANDLE h, float freq, float rate, float bandwidth)
{
    struct soapy_channel_t *chan = soapy_get_channel(h);
    chan->_sdr.freq = freq;
    chan->_sdr.rate = rate;
    chan->_sdr.bandwidth = bandwidth;
}