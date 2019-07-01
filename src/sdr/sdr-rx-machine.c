#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <radpool.h>

#include "filter.h"
#include "machine.h"
#include "sdr-machine.h"
#include "simple-buffers.h"

static pthread_mutex_t sdr_list_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sdr_channel_t *channels = NULL;
static struct sdr_device_t *devices = NULL;

/* Device Access */
static struct sdr_device_t *
get_device(IO_HANDLE h)
{
    struct sdr_device_t *d = devices;
    while (d) {
        struct sdr_channel_t *c = d->channels;
        while (c) {
            if (c->handle == h) {
                return d;
            }
            c = c->next;
        }
        d = d->next;
    }
    return NULL;
}

/* Channel Access */
static void
acquire_channel(struct sdr_channel_t *c)
{
    pthread_mutex_lock(&c->lock);
    c->in_use++;
    pthread_mutex_unlock(&c->lock);
}

static void
release_channel(struct sdr_channel_t *c)
{
    pthread_mutex_lock(&c->lock);
    c->in_use--;
    pthread_mutex_unlock(&c->lock);
}

static void
add_channel(struct sdr_device_t *dev, struct sdr_channel_t *chan)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_channel_t *c = dev->channels;
    if (!c) {
        dev->channels = chan;
    } else {
        while (c->next) {
            c = c->next;
        }
        c->next = chan;
    }
    pthread_mutex_unlock(&sdr_list_lock);
}

// Get a pointer to a channel with the specified handle
static struct sdr_channel_t *
get_channel(IO_HANDLE h)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_device_t *d = get_device(h);
    if (!d) {
        return NULL;
    }

    struct sdr_channel_t *c = d->channels;
    while (c) {
        if (c->handle == h) {
            break;
        }
        c = c->next;
    }
    pthread_mutex_unlock(&sdr_list_lock);

    return c;
}

static struct sdr_channel_t *
pop_channel(IO_HANDLE h)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_device_t *d = get_device(h);
    if (!d) {
        return NULL;
    }

    struct sdr_channel_t *c = d->channels;
    struct sdr_channel_t *cp = NULL;
    while (c) {
        if (c->handle == h) {
            break;
        }
        cp = c;
        c = c->next;
    }
    pthread_mutex_unlock(&sdr_list_lock);

    // handle not found
    if (!c) {
        return NULL;

    // handle in first slot
    } else if (!cp) {
        pthread_mutex_lock(&sdr_list_lock);
        channels = c->next;
        pthread_mutex_unlock(&sdr_list_lock);

    } else if (c && cp) {
        pthread_mutex_lock(&sdr_list_lock);
        cp->next = c->next;
        pthread_mutex_unlock(&sdr_list_lock);
    }

    return c;
}

static void
sdr_lock(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return;
    }

    pthread_mutex_lock(&c->lock);
}

static void
sdr_unlock(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return;
    }

    pthread_mutex_unlock(&c->lock);
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
    char i;
    if (d->channels) {
        return;
    }

    printf("All channels have been destroyed.  Destroying device %d.\n", d->id);

    d->destroy_device_impl(d);
    
    // Device in first slot
    if (!dp) {
        devices = d->next;

    } else if (d && dp) {
        dp->next = d->next;
    }

    pfree(d->pool);
}

static void
sdr_destroy(IO_HANDLE h)
{
    // Access the device, for reference
    struct sdr_device_t *d = get_device(h);

    // Remove the channel from operation
    struct sdr_channel_t *c = pop_channel(h);
    if (!c) {
        return;
    }

    // Wait until channel is not being used
    while (c->in_use) {
        continue;
    }

    // Call the implementation-specific destroy function
    if (c->destroy_channel_impl) {
        pthread_mutex_lock(&c->lock);
        c->destroy_channel_impl(c);
        pthread_mutex_unlock(&c->lock);
    }

    // Reset IO Handle in the device struct
    pthread_mutex_lock(&c->lock);
    c->io = 0;
    pthread_mutex_unlock(&c->lock);

    pthread_mutex_destroy(&c->lock);
    pfree(c->pool);

    pthread_mutex_lock(&sdr_list_lock);
    cleanup_device(d);
    pthread_mutex_unlock(&sdr_list_lock);
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

static struct io_desc *
sdr_rx_get_read_desc(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return NULL;
    }

    if (c->dir == SDR_CHAN_RX) {
        return c->io;
    }

    return NULL;
}

static struct io_desc *
sdr_rx_get_write_desc(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return NULL;
    }

    if (c->dir == SDR_CHAN_TX) {
        return c->io;
    }

    return NULL;
}

static int
sdr_rx_read(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_channel(c);
    struct io_filter_t *f = (struct io_filter_t *)c->io->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_channel(c);

    return status;
}

static int
sdr_rx_write(IO_HANDLE h, void *buf, uint64_t *len)
{
    printf("ERROR: sdr_rx_machine has no write function\n");
    return IO_ERROR;
}

static enum io_status
init_rx_filter(struct sdr_channel_t *chan, struct sdr_device_t *dev, sdr_filter_init rx)
{
    if (!rx) {
        printf("Failed to initialize rx filter: Null sdr_init function\n");
        return IO_ERROR;
    }

    // Create io descriptors
    chan->io = (struct io_desc *)pcalloc(chan->pool, sizeof(struct io_desc));
    if (!chan->io) {
        printf("Failed to initialize read descriptor\n");
        return IO_ERROR;
    }
    chan->io->alloc = chan->pool;

    struct io_filter_t *f = rx(chan->pool, chan, dev);
    if (!f) {
        return IO_ERROR;
    }
    chan->io->obj = f;

    return IO_SUCCESS;
}

IO_HANDLE
sdr_create(const IOM *machine, void *arg)
{
    // Get sdr api
    SDR_API *api = (SDR_API *)machine->obj;
    if (!api) {
        return 0;
    }

    POOL *var_pool = create_subpool(machine->alloc);
    if (!var_pool) {
        printf("ERROR: Failed to create memory pool(s)\n");
        return 0;
    }

    if (api->set_vars) {
        api->set_vars(var_pool, arg);
    }

    // Device Init
    POOL *device_pool = create_subpool(machine->alloc);
    if (!device_pool) {
        printf("ERROR: Failed to create memory pool(s)\n");
        return 0;
    }

    struct sdr_device_t *device = api->device(device_pool, arg);
    if (!device) {
        printf("ERROR: Failed to create device descriptor\n");
        pfree(device_pool);
        return 0;
    }
    pthread_mutex_init(&device->lock, NULL);
    device->pool = device_pool;

    // Channel Init
    POOL *channel_pool = create_subpool(machine->alloc);
    if (!channel_pool) {
        printf("ERROR: Failed to create memory pool(s)\n");
        pfree(device_pool);
        return 0;
    }

    struct sdr_channel_t *chan = api->channel(channel_pool, device, arg);
    if (!chan) {
        printf("ERROR: Failed to create a new channel\n");
        pfree(device_pool);
        pfree(channel_pool);
        return 0;
    }
    pthread_mutex_init(&chan->lock, NULL);
    chan->pool = channel_pool;
    chan->dir = SDR_CHAN_RX;

    if (init_rx_filter(chan, device, api->rx_filter) != IO_SUCCESS) {
        printf("ERROR: Failed to initialize filter\n");
        pfree(device_pool);
        pfree(channel_pool);
        return 0;
    }

    add_device(device);
    add_channel(device, chan);

    chan->handle = request_handle((IOM *)machine);
    printf("Handle for %s = %d\n", machine->name, chan->handle);

    return chan->handle;
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
    machine->get_read_desc = sdr_rx_get_read_desc;
    machine->get_write_desc = sdr_rx_get_write_desc;
    machine->stop = machine_disable_read;
    machine->destroy = sdr_destroy;
    machine->read = sdr_rx_read;
    machine->write = sdr_rx_write;
}

const struct sdr_device_t *
sdr_get_device(IO_HANDLE h)
{
    return (const struct sdr_device_t *)get_device(h);
}

struct sdr_channel_t *
sdr_get_channel(IO_HANDLE h, const struct sdr_device_t *dev)
{
    struct sdr_channel_t *chan = dev->channels;
    while (chan) {
        if (chan->handle == h) {
            return chan;
        }
        chan = chan->next;
    }

    return NULL;
}
