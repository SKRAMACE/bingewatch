#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#include "machine.h"
#include "filter.h"
#include "sdr-machine.h"
#include "simple-buffers.h"

#define LOGEX_TAG "BW-SDRRX"
#include "logging.h"
#include "bw-log.h"

#define SDR_RX_CHAN_ALLOC 10

static pthread_mutex_t sdr_list_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sdr_device_t *devices = NULL;

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
