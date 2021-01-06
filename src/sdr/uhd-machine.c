#include <stdio.h> //printf
#include <stdlib.h> //free
#include <string.h>
#include <complex.h>
#include <uhd.h>

#include "machine.h"
#include "filter.h"
#include "sdr-machine.h"
#include "sdrs.h"

#define UHD_RX_TIMEOUT 3.0
#define RECV_FRAMES ",recv_frame_size=2056,num_recv_frames=2048"

enum uhd_var_e {
    UHD_VAR_FREQ,
    UHD_VAR_RATE,
    UHD_VAR_BANDWIDTH,
};

const IOM *uhd_rx_machine;
static IOM *_uhd_rx_machine = NULL;

struct uhd_args_t {
    size_t channel;
    const char *serial;
};

// Machine Structs
struct uhd_api_t {
    SDR_API _sdr;
};

struct uhd_device_t {
    struct sdr_device_t _sdr;
    uhd_rx_streamer_handle rx_streamer;
    uhd_rx_metadata_handle rx_metadata;
};

struct uhd_channel_t {
    struct sdr_channel_t _sdr;
    uhd_usrp_handle *sdr;
    size_t channel;
    uhd_tune_request_t tune_request;
    uhd_tune_result_t tune_result;
    uhd_stream_args_t stream_args;
    uhd_stream_cmd_t stream_cmd;
    uhd_rx_streamer_handle *rx_streamer;
    uhd_rx_metadata_handle *rx_metadata;
    size_t max_samples;
};

static struct uhd_channel_t *
uhd_get_channel(IO_HANDLE h)
{
    const struct sdr_device_t *dev = sdr_get_device(h);
    struct sdr_channel_t *chan = sdr_get_channel(h, dev);
    return (struct uhd_channel_t *)chan;
}

static void
set_vars(POOL *p, void *args) {
}

// Cleanup Functions
static void
destroy_device(struct sdr_device_t *sdr)
{
    struct uhd_device_t *d = (struct uhd_device_t *)sdr;

    uhd_rx_streamer_free(&d->rx_streamer);
    uhd_rx_metadata_free(&d->rx_metadata);

    uhd_usrp_handle *usrp = (uhd_usrp_handle *)sdr->hw;
    uhd_usrp_free(usrp);
}

static int
uhd_channel_init(struct uhd_channel_t *chan)
{
    char error_string[512];

    // Set sample rate
    if (uhd_usrp_set_rx_rate(*chan->sdr, chan->_sdr.rate, chan->channel) != 0) {
        goto error;
    }

    // Set sample rate
    if (uhd_usrp_set_rx_bandwidth(*chan->sdr, chan->_sdr.bandwidth, chan->channel) != 0) {
        goto error;
    }

    // Set frequency
    chan->tune_request.target_freq = chan->_sdr.freq;
    chan->tune_request.rf_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO;
    chan->tune_request.dsp_freq_policy = UHD_TUNE_REQUEST_POLICY_AUTO;

    uhd_usrp_handle usrp = *chan->sdr;
    uhd_rx_streamer_handle rx = *chan->rx_streamer;
    
    if (uhd_usrp_set_rx_freq(usrp, &chan->tune_request, chan->channel, &chan->tune_result) != 0) {
        goto error;
    }
        
    // Set gain
    if (uhd_usrp_set_rx_gain(usrp, chan->_sdr.gain, chan->channel, "") != 0) {
        goto error;
    }

    // Start Streaming
    if (uhd_usrp_get_rx_stream(usrp, &chan->stream_args, rx) != 0) {
        goto error;
    }

    if (uhd_rx_streamer_max_num_samps(rx, &chan->max_samples) != 0) {
        goto error;
    }

    int ret = uhd_rx_streamer_issue_stream_cmd(rx, &chan->stream_cmd);
    if (ret != 0) {
    //if (uhd_rx_streamer_issue_stream_cmd(rx, &chan->stream_cmd) != 0) {
        goto error;
    }

    return IO_SUCCESS;

error:
    uhd_get_last_error(error_string, 512);
    fprintf(stderr, "USRP reported the following error: %s\n", error_string);
    uhd_usrp_last_error(*chan->sdr, error_string, 512);
    fprintf(stderr, "USRP reported the following error: %s\n", error_string);
    return IO_ERROR;
}

// Read from device
static int
read_data_from_hw(IO_FILTER_ARGS)
{
    // Dereference channel from filter object
    struct uhd_channel_t *chan = (struct uhd_channel_t *)IO_FILTER_ARGS_FILTER->obj;
    uhd_rx_streamer_handle rx = *chan->rx_streamer;

    if (chan->_sdr.error) {
        return IO_ERROR;
    }

    if (!chan->_sdr.init) {
        if (uhd_channel_init(chan) < IO_SUCCESS) {
            printf("ERROR: Failed to init channel\n");
            return IO_ERROR;
        }
        chan->_sdr.init = 1;
    }

    float complex *out = (float complex *)IO_FILTER_ARGS_BUF;
    size_t remaining = *IO_FILTER_ARGS_BYTES / sizeof(complex float);
    while (remaining > 0) {
        size_t r_samp = 1000000;
        size_t n_samp = (remaining < r_samp) ? remaining : r_samp;

        size_t read_samp;
        void **buffs = (void **)&out;
        uhd_rx_streamer_recv(rx, buffs, n_samp, chan->rx_metadata, UHD_RX_TIMEOUT, false, &read_samp);

        uhd_rx_metadata_error_code_t error_code;
        if (uhd_rx_metadata_error_code(*chan->rx_metadata, &error_code) != 0) {
            printf("ERROR: stream error (RX metadata)\n");
            return IO_ERROR;
        }

        switch (error_code) {
        case UHD_RX_METADATA_ERROR_CODE_NONE:
            printf("-");
            break;
        case UHD_RX_METADATA_ERROR_CODE_OVERFLOW:
            break;
        default:
            printf("ERROR: uhd error (%d)\n", error_code);
            return IO_ERROR;
        }

        out += read_samp;
        remaining -= read_samp;
    }

    printf("%zd bytes\n", *IO_FILTER_ARGS_BYTES);
    return IO_SUCCESS;
}

/*
 * --------------------------------------------------
 * -- UHD Device 0
 * --------------------------------------------------
 * Device Address:
 *     serial: 31A3D1E
 *     name: MyB210
 *     product: B210
 *     type: b200
 */
static struct sdr_device_t *
create_device(POOL *p, void *args)
{
    if(uhd_set_thread_priority(uhd_default_thread_priority, true)){
        printf("WARNING: Unable to set thread priority\n");
    }

    // Copy serial from args
    char *uhd_args = pcalloc(p, 1024);
    struct uhd_args_t *uarg = (struct uhd_args_t *)args;
    snprintf(uhd_args, 1024,
        "serial=%s"RECV_FRAMES, uarg->serial);

    // Attempt to connect to device by serial
    uhd_usrp_handle *sdr = (uhd_usrp_handle *)pcalloc(p, sizeof(uhd_usrp_handle));
    if (uhd_usrp_make(sdr, uhd_args) != 0) {
        printf("ERROR: uhd_usrp_make() failure\n");
        return NULL;
    }

    // Success!  Now, create the IOM structs
    struct uhd_device_t *dev = pcalloc(p, sizeof(struct uhd_device_t));
    if (!dev) {
        printf("ERROR: Failed to allocate %zu bytes for sdr device\n",
            sizeof(struct uhd_device_t));
        return NULL;
    }

    // Create RX streamer
    if (uhd_rx_streamer_make(&dev->rx_streamer) != 0) {
        printf("ERROR: uhd_rx_streamer_make() failure\n");
        return NULL;
    }

    // Create RX metadata
    if (uhd_rx_metadata_make(&dev->rx_metadata) != 0) {
        printf("ERROR: uhd_rx_metadata_make() failure\n");
        return NULL;
    }

    // Initialize generic sdr device
    struct sdr_device_t *d = (struct sdr_device_t *)dev;
    pthread_mutex_init(&d->lock, NULL);
    d->hw = sdr;
    d->destroy_device_impl = destroy_device;

    return d;
}

static struct sdr_channel_t *
create_channel(POOL *p, struct sdr_device_t *dev, void *args)
{
    struct uhd_channel_t *chan = (struct uhd_channel_t *)pcalloc(p, sizeof(struct uhd_channel_t));
    struct uhd_device_t *ud = (struct uhd_device_t *)dev;

    chan->rx_streamer = &ud->rx_streamer;
    chan->rx_metadata = &ud->rx_metadata;

    chan->stream_args.cpu_format = "fc32";
    chan->stream_args.otw_format = "sc16";
    chan->stream_args.args = "";
    chan->stream_args.channel_list = &chan->channel;
    chan->stream_args.n_channels = 1;

    chan->stream_cmd.stream_mode = UHD_STREAM_MODE_START_CONTINUOUS;
    chan->stream_cmd.num_samps = 0;
    chan->stream_cmd.stream_now = true;

    chan->sdr = dev->hw;

    struct uhd_args_t *uarg = (struct uhd_args_t *)args;
    chan->channel = uarg->channel;

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
    fhw = create_filter(p, "_uhd_hw", read_data_from_hw);
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
    struct uhd_api_t *api = (struct uhd_api_t *)pcalloc(p, sizeof(struct uhd_api_t));

    sdr_init_api_functions(machine, &api->_sdr);

    api->_sdr.set_vars = set_vars;
    api->_sdr.device = create_device;
    api->_sdr.channel = create_channel;
    api->_sdr.rx_filter = create_rx_filter;
    api->_sdr.tx_filter = NULL;
    
    machine->obj = api;
}

static IO_HANDLE
uhd_create(void *args)
{
    printf("ERROR: Implementation Error: Use \"sdr_create()\" to implement a uhd machine\n");
    return 0;
}

static int
uhd_set_val(IO_HANDLE h, int var, double val)
{
    int ret = 1;
    char error_string[512];

    struct uhd_channel_t *uhd = uhd_get_channel(h);
    uhd_usrp_handle usrp = *uhd->sdr;
    uhd_rx_streamer_handle rx = *uhd->rx_streamer;

    struct sdr_channel_t *chan = &uhd->_sdr;

    while (chan->in_use) {
        continue;
    }

    pthread_mutex_lock(&chan->lock);
    switch (var) {
    case UHD_VAR_FREQ:
        chan->freq = val; break;
    case UHD_VAR_RATE:
        chan->rate = val; break;
    case UHD_VAR_BANDWIDTH:
        chan->bandwidth = val; break;
    default:
        printf("Unknown Var (%d)\n", var);
        goto failure;
    }

    if (!chan->init) {
        goto success;
    }
    
    uhd->stream_cmd.stream_mode = UHD_STREAM_MODE_STOP_CONTINUOUS;
    uhd_rx_streamer_issue_stream_cmd(rx, &uhd->stream_cmd);

    switch (var) {
    case UHD_VAR_FREQ:
        uhd->tune_request.target_freq = val;
        if (uhd_usrp_set_rx_freq(usrp, &uhd->tune_request, uhd->channel, &uhd->tune_result) != 0) {
            goto error;
        }
        break;
    case UHD_VAR_RATE:
        if (uhd_usrp_set_rx_rate(usrp, val, uhd->channel) != 0) {
            goto error;
        }
        break;
    case UHD_VAR_BANDWIDTH:
        if (uhd_usrp_set_rx_bandwidth(usrp, val, uhd->channel) != 0) {
            goto error;
        }
        break;
    default:
        printf("Unknown Var (%d)\n", var);
        goto failure;
    }

    uhd->stream_cmd.stream_mode = UHD_STREAM_MODE_START_CONTINUOUS;
    uhd_rx_streamer_issue_stream_cmd(rx, &uhd->stream_cmd);

success:
    pthread_mutex_unlock(&chan->lock);
    return 0;

error:
    uhd_get_last_error(error_string, 512);
    fprintf(stderr, "USRP reported the following error: %s\n", error_string);
    uhd_usrp_last_error(*uhd->sdr, error_string, 512);
    fprintf(stderr, "USRP reported the following error: %s\n", error_string);
    chan->error = 1;

failure:
    pthread_mutex_unlock(&chan->lock);
    return 1;
}

int
uhd_rx_set_freq(IO_HANDLE h, double freq)
{
    return uhd_set_val(h, UHD_VAR_FREQ, freq);
}

int
uhd_rx_set_samp_rate(IO_HANDLE h, double samp_rate)
{
    return uhd_set_val(h, UHD_VAR_RATE, samp_rate);
}

int
uhd_rx_set_bandwidth(IO_HANDLE h, double bandwidth)
{
    return uhd_set_val(h, UHD_VAR_BANDWIDTH, bandwidth);
}


const IOM *
get_uhd_rx_machine()
{
    IOM *machine = _uhd_rx_machine;
    if (!machine) {
        machine = machine_register("uhd_rx");

        sdr_init_machine_functions(machine);
        api_init(machine);

        // Local Functions
        machine->create = uhd_create;

        _uhd_rx_machine = machine;
        uhd_rx_machine = machine;
    }
    return (const IOM *)machine;
}

IO_HANDLE
new_uhd_rx_machine(const char *id, int channel)
{
    const IOM *machine = get_uhd_rx_machine();
    struct uhd_args_t uarg = {
        .channel = channel,
        .serial = id,
    };

    return sdr_create(machine, &uarg);
}

IO_HANDLE
new_b210_rx_machine(const char *id)
{
    return new_uhd_rx_machine(id, 0);
}

void
uhd_set_gain(IO_HANDLE h, float gain)
{
    struct uhd_channel_t *chan = uhd_get_channel(h);
    chan->_sdr.gain = gain;
}

void
uhd_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    struct uhd_channel_t *chan = uhd_get_channel(h);
    chan->_sdr.freq = freq;
    chan->_sdr.rate = rate;
    chan->_sdr.bandwidth = bandwidth;
}
