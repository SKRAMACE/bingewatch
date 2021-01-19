#include <stdio.h> //printf
#include <stdlib.h> //free
#include <string.h>
#include <complex.h>
#include <uhd.h>

#include "filter.h"
#include "sdr-machine.h"
#include "bw-uhd.h"
#include "sdrs.h"

#define LOGEX_TAG "BW-UHD"
#include "logging.h"
#include "bw-log.h"

#define MAX_ERROR_COUNT 1
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
    char *device_string;
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

static void
set_vars(POOL *p, void *args) {
}

// Cleanup Functions
static void
destroy_device(struct sdr_device_t *sdr)
{
    struct uhd_device_t *d = (struct uhd_device_t *)sdr;

    uhd_usrp_handle *usrp = (uhd_usrp_handle *)sdr->hw;
    uhd_usrp_free(usrp);
}

static void
destroy_uhd_channel(struct sdr_channel_t *chan)
{
    struct uhd_channel_t *c = (struct uhd_channel_t *)chan;

    uhd_rx_streamer_free(&c->rx_streamer);
    uhd_rx_metadata_free(&c->rx_metadata);
}

static void
usrp_print_error(struct uhd_channel_t *chan)
{
    char *uhd_error = malloc(1024);
    uhd_get_last_error(uhd_error, 1024);

    char *usrp_error = malloc(1024);
    uhd_usrp_last_error(*chan->sdr, usrp_error, 1024);
    error("UHD: \"%s\", USRP: \"%s\"", uhd_error, usrp_error);

    return;
}

static int
uhd_channel_set(struct uhd_channel_t *chan)
{
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
    
    if (uhd_usrp_set_rx_freq(usrp, &chan->tune_request, chan->channel, &chan->tune_result) != 0) {
        goto error;
    }
        
    // Set gain
    if (uhd_usrp_set_rx_gain(usrp, chan->_sdr.gain, chan->channel, "") != 0) {
        goto error;
    }

    // Start Streaming
    if (uhd_usrp_get_rx_stream(usrp, &chan->stream_args, chan->rx_streamer) != 0) {
        goto error;
    }

    if (uhd_rx_streamer_max_num_samps(chan->rx_streamer, &chan->max_samples) != 0) {
        goto error;
    }

    return IO_SUCCESS;

error:
    usrp_print_error(chan);
    return IO_ERROR;
}

static int
uhd_channel_start(struct uhd_channel_t *chan)
{
    int ret = uhd_rx_streamer_issue_stream_cmd(chan->rx_streamer, &chan->stream_cmd);
    if (ret != 0) {
        goto error;
    }

    // TODO: RUN UNTIL NO OVERFLOWS
    //uhd_rx_streamer_recv(chan->rx_streamer, buffs, n_samp, chan->rx_metadata, UHD_RX_TIMEOUT, false, &read_samp);

    return IO_SUCCESS;

error:
    usrp_print_error(chan);
    return IO_ERROR;
}

// Read from device
static int
read_data_from_hw(IO_FILTER_ARGS)
{
    // Dereference channel from filter object
    struct uhd_channel_t *chan = (struct uhd_channel_t *)IO_FILTER_ARGS_FILTER->obj;
    struct sdr_channel_t *sdr = (struct sdr_channel_t *)chan;

    switch (sdr->state) {
    case SDR_CHAN_ERROR:
        return IO_ERROR;
    case SDR_CHAN_NOINIT:
        if (uhd_channel_set(chan) < IO_SUCCESS) {
            error("Failed to set soapy channel");
            return IO_ERROR;
        }

        if (uhd_channel_start(chan) < IO_SUCCESS) {
            error("Failed to start soapy channel");
            return IO_ERROR;
        }

        sdr->state = SDR_CHAN_READY;
        break;

    case SDR_CHAN_READY:
        break;
    default:
        error("Unknown sdr channel state \"%d\"", sdr->state);
        return IO_ERROR;
    }

    float complex *data = (float complex *)IO_FILTER_ARGS_BUF;
    size_t remaining = *IO_FILTER_ARGS_BYTES / sizeof(complex float);
    while (remaining > 0) {
        size_t n_samp = remaining;

        size_t read_samp;
        void **buffs = (void **)&data;
        uhd_rx_streamer_recv(chan->rx_streamer, buffs, n_samp, &chan->rx_metadata, UHD_RX_TIMEOUT, false, &read_samp);

        uhd_rx_metadata_error_code_t error_code;
        if (uhd_rx_metadata_error_code(chan->rx_metadata, &error_code) != 0) {
            error("Stream error (RX metadata)");
            return IO_ERROR;
        }

        int overflow = 0;
        switch (error_code) {
        case UHD_RX_METADATA_ERROR_CODE_NONE:
            break;

        case UHD_RX_METADATA_ERROR_CODE_OVERFLOW:
            overflow = 1;
            break;

        case UHD_RX_METADATA_ERROR_CODE_TIMEOUT:
            error("Timeout");
            return IO_ERROR;

        case UHD_RX_METADATA_ERROR_CODE_LATE_COMMAND:
            error("Late command");
            return IO_ERROR;

        case UHD_RX_METADATA_ERROR_CODE_BROKEN_CHAIN:
            error("Broken chain");
            return IO_ERROR;

        case UHD_RX_METADATA_ERROR_CODE_ALIGNMENT:
            error("Multi-channel alignment failed");
            return IO_ERROR;

        case UHD_RX_METADATA_ERROR_CODE_BAD_PACKET:
            error("Bad packet");
            return IO_ERROR;

        default:
            error("uhd error (%d)", error_code);
            return IO_ERROR;
        }

        if (overflow) {
            if (sdr->allow_overruns) {
                warn("overflow");
            } else {
                error("overflow");
                goto overflow_error;
            }
        } else {
            chan->error_counter = 0;
        }

        data += read_samp;
        remaining -= read_samp;
    }

    return IO_SUCCESS;

overflow_error:
    *IO_FILTER_ARGS_BYTES = 0;
    return (++chan->error_counter > MAX_ERROR_COUNT) ? IO_ERROR : IO_SUCCESS;
}

/* Example device info
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
    if (uhd_set_thread_priority(uhd_default_thread_priority, true)){
        warn("Unable to set thread priority");
    }

    // Dereference args
    struct uhd_args_t *uarg = (struct uhd_args_t *)args;

    // Check for malformed device string
    if (strlen(uarg->device_string) > 4096) {
        error("Device string too long (%zd characters)", strlen(uarg->device_string));
        return NULL;
    }

    // Allocate space for device string
    size_t ds_len = strlen(uarg->device_string) + 1;
    char *dev_str = (char *)pcalloc(p, ds_len);
    strncpy(dev_str, uarg->device_string, ds_len);
    dev_str[ds_len - 1] = 0;

    uhd_usrp_handle *sdr = (uhd_usrp_handle *)pcalloc(p, sizeof(uhd_usrp_handle));
    if (uhd_usrp_make(sdr, dev_str) != 0) {
        char *uhd_error = malloc(1024);
        uhd_get_last_error(uhd_error, 1024);
        debug("%s", uhd_error);
        free(uhd_error);

        error("uhd_usrp_make() failed for --args=\"%s\"", uarg->device_string);
        return NULL;
    }

    // Success!  Now, create the IOM structs
    struct uhd_device_t *dev = pcalloc(p, sizeof(struct uhd_device_t));
    if (!dev) {
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

    // Create RX streamer
    if (uhd_rx_streamer_make(&chan->rx_streamer) != 0) {
        error("uhd_rx_streamer_make() failure");
        return NULL;
    }

    // Create RX metadata
    if (uhd_rx_metadata_make(&chan->rx_metadata) != 0) {
        error("uhd_rx_metadata_make() failure");
        return NULL;
    }

    chan->stream_args.cpu_format = "fc32";
    chan->stream_args.otw_format = "sc16";
    chan->stream_args.args = "";
    chan->stream_args.channel_list = &chan->channel;
    chan->stream_args.n_channels = 1;

    chan->stream_cmd.stream_mode = UHD_STREAM_MODE_START_CONTINUOUS;
    chan->stream_cmd.num_samps = 0;
    chan->stream_cmd.stream_now = true;

    chan->sdr = dev->hw;
    chan->_sdr.destroy_channel_impl = destroy_uhd_channel;

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
        error("Failed to create uhd rx filters");
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
    error("Implementation Error: Use \"sdr_create()\" to implement a uhd machine");
    return 0;
}

static int
uhd_set_val(IO_HANDLE h, int var, double val)
{
    int ret = 1;
    char error_string[512];

    struct uhd_channel_t *uhd = uhd_get_channel(h);
    uhd_usrp_handle usrp = *uhd->sdr;

    struct sdr_channel_t *chan = &uhd->_sdr;
    IO_DESC *d = (IO_DESC *)chan;

    while (d->in_use) {
        continue;
    }

    pthread_mutex_lock(&d->lock);
    switch (var) {
    case UHD_VAR_FREQ:
        chan->freq = val; break;
    case UHD_VAR_RATE:
        chan->rate = val; break;
    case UHD_VAR_BANDWIDTH:
        chan->bandwidth = val; break;
    default:
        error("Unknown Var (%d)", var);
        goto failure;
    }

    if (chan->state == SDR_CHAN_NOINIT) {
        goto success;
    }
    
    uhd->stream_cmd.stream_mode = UHD_STREAM_MODE_STOP_CONTINUOUS;
    uhd_rx_streamer_issue_stream_cmd(uhd->rx_streamer, &uhd->stream_cmd);

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
        error("Unknown Var (%d)", var);
        goto failure;
    }

    uhd->stream_cmd.stream_mode = UHD_STREAM_MODE_START_CONTINUOUS;
    uhd_rx_streamer_issue_stream_cmd(uhd->rx_streamer, &uhd->stream_cmd);

success:
    pthread_mutex_unlock(&d->lock);
    return 0;

error:
    usrp_print_error(uhd);
    chan->state = SDR_CHAN_ERROR;

failure:
    pthread_mutex_unlock(&d->lock);
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

static void
uhd_log_init()
{
    char lvl[64];
    ENVEX_COPY(lvl, 64, "BW_B210_LOG_LEVEL", "error");
    b210_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_UHD_LOG_LEVEL", "error");
    uhd_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_SDRRX_LOG_LEVEL", "error");
    sdrrx_set_log_level(lvl);
}

IOM *
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

        uhd_log_init();
    }
    return (IOM *)machine;
}

IO_HANDLE
new_uhd_rx_machine_devstr(int channel, char *devstr)
{
    IOM *machine = get_uhd_rx_machine();
    struct uhd_args_t uarg = {
        .channel = channel,
        .device_string = devstr,
    };

    return sdr_create(machine, &uarg);
}

IO_HANDLE
new_uhd_rx_machine(int channel)
{
    new_uhd_rx_machine_devstr(channel, "");
}

struct uhd_channel_t *
uhd_get_channel(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("UHD channel %d not found", h);
        return NULL;
    }

    return (struct uhd_channel_t *)d;
}

void
uhd_rx_set_gain(IO_HANDLE h, float lna)
{
    struct uhd_channel_t *chan = uhd_get_channel(h);
    chan->_sdr.gain = lna;
}

void
uhd_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth)
{
    struct uhd_channel_t *chan = uhd_get_channel(h);
    if (!chan) {
        return;
    }

    chan->_sdr.freq = freq;
    chan->_sdr.rate = rate;
    chan->_sdr.bandwidth = bandwidth;
}

void
uhd_set_log_level(char *level)
{
    sdrrx_set_log_level(level);
    bw_set_log_level_str(level);
}
