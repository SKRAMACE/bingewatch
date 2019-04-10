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

// Add a new channel descriptor
void
add_channel(struct sdr_channel_t *chan)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_channel_t *c = channels;
    if (!c) {
        channels = chan;
    } else {
        while (c->next) {
            c = c->next;
        }
        c->next = chan;
    }
    pthread_mutex_unlock(&sdr_list_lock);
}

// Get a pointer to a channel with the specified handle
struct sdr_channel_t *
get_channel(IO_HANDLE h)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_channel_t *c = channels;

    while (c) {
        IO_HANDLE ch = c->handle;

        if (ch == h) {
            break;
        }

        c = c->next;
    }
    pthread_mutex_unlock(&sdr_list_lock);

    return c;
}

struct sdr_channel_t *
pop_channel(IO_HANDLE h)
{
    pthread_mutex_lock(&sdr_list_lock);
    struct sdr_channel_t *c = channels;

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

void
sdr_channel_lock(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return;
    }

    pthread_mutex_lock(&c->lock);
}

void
sdr_channel_unlock(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return;
    }

    pthread_mutex_unlock(&c->lock);
}

static void
destroy_channel(IO_HANDLE h)
{
    // Remove the channel from operation
    struct sdr_channel_t *c = pop_channel(h);
    if (!c) {
        return;
    }

    // Wait, if the channel is in use
    while (c->in_use) {
        continue;
    }

    // Call the implementation-specific destroy function
    if (c->destroy_channel_impl) {
        pthread_mutex_lock(&c->lock);
        c->destroy_channel_impl(c);
        pthread_mutex_unlock(&c->lock);
    }

    // Access the HW struct
    struct sdr_device_t *d = c->device;

    // Reset IO Handle in the device struct
    pthread_mutex_lock(&c->lock);
    d->io_handles[c->num] = 0;
    pthread_mutex_unlock(&c->lock);

    pthread_mutex_destroy(&c->lock);
    pfree(c->pool);

    pthread_mutex_lock(&sdr_list_lock);
    cleanup_device(d);
    pthread_mutex_unlock(&sdr_list_lock);
}

/* Device Access */
// Add a new device descriptor
void
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

struct sdr_device_t *
get_device(char id)
{
    struct sdr_device_t *d = devices;
    while (d) {
        if (d->id == id) {
            return d;
        }
        d = d->next;
    }
    return NULL;
}

void
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

    // Preserve the device until all io handles are destroyed
    char i;
    for (i = 0; i < d->io_handle_count; i++) {
        if (d->io_handles[i]) {
            return;
        }
    }

    printf("All io handles have been destroyed.  Destroying device %d.\n", d->id);

    // Device in first slot
    if (!dp) {
        devices = d->next;

    } else if (d && dp) {
        dp->next = d->next;
    }

    pfree(d->pool);
}

struct io_desc *
sdr_get_rx_desc(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return NULL;
    }

    return c->io_rx;
}

struct io_desc *
sdr_get_tx_desc(IO_HANDLE h)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        return NULL;
    }

    return c->io_tx;
}

int
sdr_read(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_channel(c);
    struct io_filter_t *f = (struct io_filter_t *)c->io_rx->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_channel(c);

    return status;
}

int
sdr_write(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct sdr_channel_t *c = get_channel(h);
    if (!c) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_channel(c);
    struct io_filter_t *f = (struct io_filter_t *)c->io_tx->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_channel(c);

    return status;
}

enum io_status
init_rx_filter(struct sdr_channel_t *chan, sdr_init rx)
{
    if (!rx) {
        printf("Failed to initialize rx filter: Null sdr_init function\n");
        return IO_ERROR;
    }

    // Create io descriptors
    chan->io_rx = (struct io_desc *)pcalloc(chan->pool, sizeof(struct io_desc));
    if (!chan->io_rx) {
        printf("Failed to initialize read descriptor\n");
        return IO_ERROR;
    }
    chan->io_rx->alloc = chan->pool;

    struct io_filter_t *f = rx(chan->pool, chan);
    if (!f) {
        return IO_ERROR;
    }
    chan->io_rx->obj = f;

    return IO_SUCCESS;
}

enum io_status
init_tx_filter(struct sdr_channel_t *chan, sdr_init tx)
{
    if (!tx) {
        printf("Failed to initialize tx filter: Null sdr_init function\n");
        return IO_ERROR;
    }

    chan->io_tx = (struct io_desc *)pcalloc(chan->pool, sizeof(struct io_desc));
    if (!chan->io_tx) {
        printf("Failed to initialize read descriptor\n");
        return IO_ERROR;
    }
    chan->io_tx->alloc = chan->pool;

    struct io_filter_t *f = tx(chan->pool, chan);
    if (!f) {
        return IO_ERROR;
    }
    chan->io_tx->obj = f;

    return IO_SUCCESS;
}

/*
 * Create new filters to interface directly with the hardware 
 */
enum io_status
init_filters(struct sdr_channel_t *chan, sdr_init rx, sdr_init tx)
{
    if (rx) {
        if (init_rx_filter(chan, rx) != IO_SUCCESS) {
            return IO_ERROR;
        }
    }

    if (tx) {
        if (init_tx_filter(chan, tx) != IO_SUCCESS) {
            return IO_ERROR;
        }
    }

    return IO_SUCCESS;
}

IO_HANDLE
create_sdr(IOM *machine, void *arg)
{
    // Get sdr functions from IOM object
    SDR *sdr = (SDR *)machine->obj;
    if (!sdr) {
        return 0;
    }

    sdr->set_vars(arg);

    // Device Init
    POOL *device_pool = create_subpool(machine->alloc);
    if (!device_pool) {
        printf("ERROR: Failed to create memory pool(s)\n");
        return 0;
    }

    struct sdr_device_t *device = sdr->device(device_pool, arg);
    if (!device) {
        printf("ERROR: Failed to create device descriptor\n");
        pfree(device_pool);
        return 0;
    }
    device->pool = device_pool;

    // Channel Init
    POOL *channel_pool = create_subpool(machine->alloc);
    if (!channel_pool) {
        printf("ERROR: Failed to create memory pool(s)\n");
        pfree(device_pool);
        return 0;
    }

    struct sdr_channel_t *chan = sdr->channel(channel_pool, arg);
    if (!chan) {
        printf("ERROR: Failed to create a new channel\n");
        pfree(device_pool);
        pfree(channel_pool);
        return 0;
    }

    pthread_mutex_init(&chan->lock, NULL);
    chan->pool = channel_pool;
    chan->direction = SDR_CHAN_NOINIT;

    chan->device = device;

    if (init_filters(chan, sdr->rx_filter, sdr->tx_filter) != IO_SUCCESS) {
        printf("ERROR: Failed to initialize filters\n");
        pfree(device_pool);
        pfree(channel_pool);
        return 0;
    }

    add_device(device);
    add_channel(chan);

    chan->handle = request_handle(machine);
    printf("Handle for %s = %d\n", machine->name, chan->handle);
    return chan->handle;
}

void
sdr_machine_register(IOM *machine)
{
    machine->lock = sdr_channel_lock;
    machine->stop = machine_disable_read;
    machine->destroy = destroy_channel;
    machine->unlock = sdr_channel_unlock;
    machine->get_read_desc = sdr_get_rx_desc;
    machine->get_write_desc = sdr_get_tx_desc;
    machine->read = sdr_read;
    machine->write = sdr_write;
}
