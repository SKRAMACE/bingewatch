#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <complex.h>
#include <unistd.h>

#include "machine.h"
#include "filter.h"
#include "sdr-machine.h"
#include "simple-buffers.h"
#include "block-list-buffer.h"

#define LOGEX_TAG "BW-SDRRX"
#include "logging.h"
#include "bw-log.h"

#define SDR_RX_CHAN_ALLOC 10

static pthread_mutex_t sdr_list_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sdr_device_t *devices = NULL;

struct hw_fill_t {
    int do_fill;
    struct sdr_channel_t *chan;
};

/* Device Access */
static struct sdr_channel_t *
get_channel(IO_HANDLE h)
{
    struct sdr_channel_t *ret = NULL;

    // Get device list
    struct sdr_device_t *d = devices;

    while (d && !ret) {
        pthread_mutex_lock(&d->lock);

        for (int i = 0; i < d->n_chan; i++) {
            IO_DESC *chan = (IO_DESC *)d->channels[i];
            if (chan->handle == h) {
                ret = (struct sdr_channel_t *)chan;
                break;
            }
        }

        pthread_mutex_unlock(&d->lock);
        d = d->next;
    }

    return ret;
}

static void
add_channel(struct sdr_device_t *dev, struct sdr_channel_t *chan)
{
    pthread_mutex_lock(&sdr_list_lock);
    if (dev->n_chan >= dev->chan_len) {
        dev->chan_len += SDR_RX_CHAN_ALLOC;
        size_t bytes = sizeof(struct sdr_channel_t *) * dev->chan_len;

        if (dev->chan_len == SDR_RX_CHAN_ALLOC) {
            dev->channels = pcalloc(dev->pool, bytes);
        } else {
            dev->channels = repalloc(dev->channels, bytes, dev->pool);
        }
    }

    dev->channels[dev->n_chan++] = chan;
    chan->device = dev;

    pthread_mutex_unlock(&sdr_list_lock);
}

static void
remove_channel_from_device(struct sdr_channel_t *chan)
{
    struct sdr_device_t *dev = chan->device;
    if (!dev) {
        return;
    }

    IO_DESC *c = (IO_DESC *)chan;
    int c_index = -1;

    // Find channel index
    pthread_mutex_lock(&dev->lock);
    for (int i = 0; i < dev->n_chan; i++) {
        IO_DESC *_c = (IO_DESC *)dev->channels[i];
        if (c->handle == _c->handle) {
            c_index = i;
            break;
        }
    }
    pthread_mutex_unlock(&dev->lock);

    if (c_index < 0) {
        error("Failed to remove channel from device");
        return;
    }

    // Remove channel from device
    pthread_mutex_lock(&dev->lock);
    dev->n_chan--;
    for (int i = c_index; i < dev->n_chan; i++) {
        dev->channels[i] = dev->channels[i + 1];
    }
    pthread_mutex_unlock(&dev->lock);
}

static void
sdr_lock(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return;
    }

    IO_DESC *d = (IO_DESC *)c;
    pthread_mutex_lock(&d->lock);
}

static void
sdr_unlock(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return;
    }

    IO_DESC *d = (IO_DESC *)c;
    pthread_mutex_unlock(&d->lock);
}

static void
cleanup_device(struct sdr_device_t *device)
{
    struct sdr_device_t *d = devices; 
    struct sdr_device_t *dp = NULL; 
    while (d) {
        if (d == device) {
            break;
        }
        dp = d;
        d = d->next;
    }

    // Device not found
    if (!d) {
        return;
    }

    // Preserve the device until all io channels are destroyed
    if (d->n_chan > 0) {
        return;
    }

    info("All channels have been destroyed.  Destroying device %d.", d->id);
    d->destroy_device_impl(d);
    
    // Device in first slot
    if (!dp) {
        devices = d->next;

    } else if (d && dp) {
        dp->next = d->next;
    }

    free_pool(d->pool);
}

static void
sdr_destroy(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Sdr channel %d not found", h);
        return;
    }
    struct sdr_channel_t *c = (struct sdr_channel_t *)d;
    struct sdr_device_t *dev = c->device;

    remove_channel_from_device(c);

    // Call the implementation-specific destroy function
    if (c->destroy_channel_impl) {
        pthread_mutex_lock(&d->lock);
        c->destroy_channel_impl(c);
        pthread_mutex_unlock(&d->lock);
    }
    machine_destroy_desc(h);
    cleanup_device(dev);
}

/* Device Access */
// Add a new device descriptor
static void
add_device(struct sdr_device_t *dev)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_device_t *d = devices;
    if (!d) {
        devices = dev;
    } else {
        while (d->next) {
            d = d->next;
        }
        d->next = dev;
    }
    pthread_mutex_unlock(&sdr_list_lock);
}

static int
sdr_rx_write(IO_HANDLE h, void *buf, size_t *len)
{
    error("sdr_rx_machine has no write function");
    return IO_ERROR;
}

static enum io_status
init_filters(struct sdr_channel_t *chan, struct sdr_device_t *dev, sdr_filter_init rx)
{
    if (!rx) {
        error("Failed to initialize rx filter: Null sdr_init function");
        return IO_ERROR;
    }

    IO_DESC *d = (IO_DESC *)chan;

    // Create io descriptors
    d->io_read = (struct io_desc *)pcalloc(d->pool, sizeof(struct io_desc));
    if (!d->io_read) {
        error("Failed to initialize read descriptor");
        return IO_ERROR;
    }
    d->io_read->alloc = d->pool;

    struct io_filter_t *f = rx(d->pool, chan, dev);
    if (!f) {
        return IO_ERROR;
    }
    d->io_read->obj = f;

    // No write filter in sdr-rx-machine
    d->io_write = NULL;

    return IO_SUCCESS;
}

IO_HANDLE
sdr_create(IOM *machine, void *arg)
{
    // Get sdr api
    SDR_API *api = (SDR_API *)machine->obj;
    if (!api) {
        return 0;
    }

    POOL *var_pool = create_subpool(machine->alloc);
    if (!var_pool) {
        error("Failed to create sdr rx memory pool");
        return 0;
    }

    if (api->set_vars) {
        api->set_vars(var_pool, arg);
    }

    // Device Init
    POOL *device_pool = create_subpool(machine->alloc);
    if (!device_pool) {
        error("Failed to create sdr rx device memory pool");
        return 0;
    }

    struct sdr_device_t *device = api->device(device_pool, arg);
    if (!device) {
        error("Failed to create sdr rx device descriptor");
        free_pool(device_pool);
        return 0;
    }
    pthread_mutex_init(&device->lock, NULL);
    device->pool = device_pool;

    // Channel Init
    POOL *channel_pool = create_subpool(machine->alloc);
    if (!channel_pool) {
        error("Failed to create sdr rx channel memory pool");
        free_pool(device_pool);
        return 0;
    }

    struct sdr_channel_t *chan = api->channel(channel_pool, device, arg);
    if (!chan) {
        error("Failed to create new sdr rx channel");
        free_pool(device_pool);
        free_pool(channel_pool);
        return 0;
    }

    if (machine_desc_init(channel_pool, machine, (IO_DESC *)chan) < IO_SUCCESS) {
        error("Failed to initialize mechine descriptor");
        free_pool(device_pool);
        free_pool(channel_pool);
        return 0;
    }

    if (init_filters(chan, device, api->rx_filter) < IO_SUCCESS) {
        error("Failed to initialize sdr rx filter");
        free_pool(device_pool);
        free_pool(channel_pool);
        return 0;
    }

    add_device(device);
    add_channel(device, chan);

    IO_HANDLE h;
    machine_register_desc((IO_DESC *)chan, &h);

    device->handle = request_handle(machine);
    info("Created device %d", device->handle);
    info("Created channel %d", h);

    return h;
}

static void
set_freq(IO_HANDLE h, void *freq)
{
    struct sdr_channel_t *chan = get_channel(h);
    chan->freq = *(float *)freq;
}

static void
set_rate(IO_HANDLE h, void *rate)
{
    struct sdr_channel_t *chan = get_channel(h);
    chan->rate = *(float *)rate;
}

static void
set_gain(IO_HANDLE h, void *gain)
{
    struct sdr_channel_t *chan = get_channel(h);
    chan->gain = *(float *)gain;
}

static void *
fill_from_hw(void *args)
{
    // Allocate return value
    int *rval = malloc(sizeof(int));
    *rval = IO_SUCCESS;

    // Dereference structs
    struct hw_fill_t *hw = (struct hw_fill_t *)args;
    struct sdr_channel_t *chan = (struct sdr_channel_t *)hw->chan;
    struct blb_rw_t *rw = (struct blb_rw_t *)chan->buffer;

    // Get write pointer
    struct __block_t *wp = rw->wp;
    pthread_mutex_lock(&rw->wlock);

    // Get api struct
    IO_DESC *d = (IO_DESC *)chan;
    SDR_API *api = (SDR_API *)d->machine->obj;

    // Copy samples from hardware to input buffer until out of space
    //      "do_fill" is managed by caller
    while (hw->do_fill && wp->bytes == 0) {
        size_t n_samp = wp->size / sizeof(float complex);
        int ret = api->hw_read(hw->chan, (complex float *)wp->data, &n_samp);
        if (ret != IO_SUCCESS) {
            *rval = ret;
            goto do_exit;
        }

        float *data_f = (float *)wp->data;
        size_t N = n_samp;
        if (data_f[0] < 16777216 && data_f[N-1] >= 16777216) {
            for (size_t i = 0; i < N; i++) {
                if (data_f[i] == 16777216) {
                    debug("hello");
                }
            }
        }

        wp->bytes = n_samp * sizeof(float complex);
        wp = wp->next;
    }

do_exit:
    rw->wp = wp;
    pthread_mutex_unlock(&rw->wlock);
    pthread_exit((void *)rval);
}

static int
read_from_buffer(struct sdr_channel_t *chan, float complex *buf, size_t *n_samp)
{
    // Dereference structs
    struct blb_rw_t *rw = (struct blb_rw_t *)chan->buffer; 

    // Init pthread arg struct
    if (!chan->obj) {
        IO_DESC *d = (IO_DESC *)chan;
        struct hw_fill_t *hw = (struct hw_fill_t *)pcalloc(d->pool, sizeof(struct hw_fill_t));
        hw->chan = chan;
        chan->obj = hw;
    }

    // Start filling buffer
    struct hw_fill_t *hw = (struct hw_fill_t *)chan->obj;
    hw->do_fill = 1;
    pthread_t fill;
    pthread_create(&fill, NULL, fill_from_hw, (void *)hw);

    // Get read pointer
    struct __block_t *rp = rw->rp;
    pthread_mutex_lock(&rw->rlock);

    // Copy samples from input buffer
    float complex *data = buf;
    size_t remaining = *n_samp;
    while (remaining) {
        // Wait for data
        if (rp->bytes == 0) {
            usleep(1000);
            continue;
        }

        size_t rp_samp = rp->bytes / sizeof(float complex);
        size_t n = (remaining >= rp_samp) ? rp_samp : remaining;
        size_t bytes = n * sizeof(float complex);
        memcpy(data, rp->data, bytes);

        float *data_f = (float *)data;
        size_t N = bytes / sizeof(float);
        if (data_f[0] < 16777216 && data_f[N-1] >= 16777216) {
            for (size_t i = 0; i < N; i++) {
                if (data_f[i] == 16777216) {
                    debug("hello");
                }
            }
        }

        data += n;
        remaining -= n;

        // If all bytes were consumed, go to next buffer
        if (rp->bytes - bytes == 0) {
            rp->bytes = 0;
            rp = rp->next;

        // If bytes are remaining, copy to front of buffer
        } else {
            char *src = rp->data + bytes;
            rp->bytes -= bytes;
            memcpy(rp->data, src, rp->bytes);
        }
    }
    rw->rp = rp;
    pthread_mutex_unlock(&rw->rlock);

    // Stop filling buffer when read is complete
    hw->do_fill = 0;

    // Join pthread and get return value
    int *ret_p;
    pthread_join(fill, (void **)&ret_p);

    int ret = *ret_p;
    free(ret_p);

    return ret;
}

int
sdrrx_read(IO_FILTER_ARGS)
{
    int ret = IO_ERROR;

    // Dereference channel from filter object
    struct sdr_channel_t *chan = (struct sdr_channel_t *)IO_FILTER_ARGS_FILTER->obj;

    // Get api struct
    IO_DESC *d = (IO_DESC *)chan;
    SDR_API *api = (SDR_API *)d->machine->obj;

    switch (chan->state) {
    case SDR_CHAN_ERROR:
        goto do_return;
    case SDR_CHAN_NOINIT:
        if (api->channel_set(chan) < IO_SUCCESS) {
            error("Failed to set channel");
            goto do_return;
        }

    case SDR_CHAN_RESET:
        if (api->channel_start(chan) < IO_SUCCESS) {
            error("Failed to start channel");
            error("Failed to start channel");
            goto do_return;
        }

        chan->state = SDR_CHAN_READY;
        break;

    case SDR_CHAN_READY:
        break;

    default:
        error("Unknown sdr channel state \"%d\"", chan->state);
        goto do_return;
    }

    float complex *data = (float complex *)IO_FILTER_ARGS_BUF;
    size_t total_samples = *IO_FILTER_ARGS_BYTES / sizeof(complex float);
    size_t n_samp = total_samples;

    switch (chan->mode) {
    case SDR_MODE_UNBUFFERED:
        ret = api->hw_read(chan, data, &n_samp);
        //ret = read_from_hw(soapy_chan, data, &n_samp);
        break;
    case SDR_MODE_BUFFERED:
        ret = read_from_buffer(chan, data, &n_samp);
        break;
    default:
        error("Invalid SDR Mode (%d)");
        goto do_return;
    }

do_return:
    if (ret != IO_SUCCESS) {
        *IO_FILTER_ARGS_BYTES = 0;
    } else {
        *IO_FILTER_ARGS_BYTES = n_samp * sizeof(float complex);
    }

    return ret;

}

int
sdrrx_read_from_counter(struct sdr_channel_t *sdr, void *buf, size_t *n_samp)
{
    float *data = (float *)buf;

    size_t N = *n_samp * 2;
    for (size_t i = 0; i < N; i++) {
        data[i] = (float)sdr->counter;
        sdr->counter = (sdr->counter + 1) % 0x1000000;
    }

    return IO_SUCCESS;
}

void
sdr_init_api_functions(IOM *machine, SDR_API *api)
{
    api->set_freq = set_freq;
    api->set_rate = set_rate;
    api->set_gain = set_gain;
}

void
sdr_init_machine_functions(IOM *machine)
{
    machine->lock = sdr_lock;
    machine->unlock = sdr_unlock;
    machine->destroy = sdr_destroy;
    machine->write = sdr_rx_write;
}

void
sdrrx_reset(IO_HANDLE h) {
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Sdr channel %d not found", h);
        return;
    }

    SDR_API *api = (SDR_API *)d->machine->obj;

    struct sdr_channel_t *chan = (struct sdr_channel_t *)d;
    if (api->channel_reset(chan) < IO_SUCCESS) {
        error("Failed to reset channel");
        return;
    }

    struct blb_rw_t *rw = (struct blb_rw_t *)chan->buffer;
    blb_rw_empty(rw);
}

static void
sdrrx_enable_buffering_bytes(IO_HANDLE h, size_t bytes_per_block, size_t n_blocks)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Sdr channel %d not found", h);
        return;
    }

    struct sdr_channel_t *c = (struct sdr_channel_t *)d;
    c->mode = SDR_MODE_BUFFERED;
    c->buffer = blb_init_rw(d->pool, bytes_per_block, n_blocks);
}

void
sdrrx_enable_buffering(IO_HANDLE h, size_t samp_per_block, size_t n_blocks)
{
    size_t block_bytes = (size_t)(samp_per_block * sizeof(float complex));

    // Call byte function
    sdrrx_enable_buffering_bytes(h, block_bytes, n_blocks);
}

void
sdrrx_enable_buffering_rate(IO_HANDLE h, double rate)
{
    // Number of seconds for entire buffer
    double sec;
    ENVEX_DOUBLE(sec, "BW_SDR_BUFFER_TIME", 1.0);

    // Number of seconds per block
    double step;
    ENVEX_DOUBLE(step, "BW_SDR_BUFFER_TIMESTEP", 0.1);

    if (sec < step) {
        error("Assert: BW_SDR_BUFFER_TIME >= BW_SDR_BUFFER_TIMESTEP");
    }

    // Calculate number of blocks
    double total_samples = rate * sec;
    double block_samples = rate * step;
    size_t n_block = (size_t)(total_samples / block_samples) + 1;
    size_t block_bytes = (size_t)(block_samples * sizeof(float complex));

    // Call byte function
    sdrrx_enable_buffering_bytes(h, block_bytes, n_block);
}

void
sdrrx_allow_overruns(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Sdr channel %d not found", h);
        return;
    }

    info("%d: Overruns allowed", h);
    struct sdr_channel_t *c = (struct sdr_channel_t *)d;

    c->allow_overruns = 1;
}

void
sdrrx_set_log_level(char *level)
{
    bw_set_log_level_str(level);
}
