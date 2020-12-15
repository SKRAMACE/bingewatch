#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "machine.h"
#include "filter.h"

#define LOGEX_TAG "BW-MACHINE"
#include "logging.h"
#include "bw-log.h"

#define machine_error(d, h, x, ...) error("%s %d: " x, d->machine->name, h, ##__VA_ARGS__)
#define machine_warn(d, h, x, ...) warn("%s %d: " x, d->machine->name, h, ##__VA_ARGS__)
#define machine_trace(d, h, x, ...) trace("%s %d: " x, d->machine->name, h, ##__VA_ARGS__)

struct machine_desc_t *machine_descriptors = NULL;
static pthread_mutex_t machine_desc_list_lock = PTHREAD_MUTEX_INITIALIZER;

// Add a new descriptor
void
machine_register_desc(struct machine_desc_t *addme, IO_HANDLE *handle)
{
    // Get a handle, and set in descriptor, and filter objects
    IO_HANDLE h = request_handle(addme->machine);
    if (h == 0) {
        *handle = 0;
        return;
    }
    addme->handle = h;

    if (addme->io_read) {
        struct io_filter_t *f = (struct io_filter_t *)addme->io_read->obj;
        f->obj = &addme->handle;
    }

    if (addme->io_write) {
        struct io_filter_t *f = (struct io_filter_t *)addme->io_write->obj;
        f->obj = &addme->handle;
    }

    *handle = h;

    pthread_mutex_lock(&machine_desc_list_lock);
    struct machine_desc_t *d = machine_descriptors;
    if (!d) {
        machine_descriptors = addme;
    } else {
        while (d->next) {
            d = d->next;
        }
        d->next = addme;
    }
    pthread_mutex_unlock(&machine_desc_list_lock);
}

// Get a pointer to a descriptor with the specified handle
struct machine_desc_t *
machine_get_desc(IO_HANDLE h)
{
    pthread_mutex_lock(&machine_desc_list_lock);
    struct machine_desc_t *d = machine_descriptors;

    while (d) {
        IO_HANDLE dh = d->handle;

        if (dh == h) {
            break;
        }

        d = d->next;
    }
    pthread_mutex_unlock(&machine_desc_list_lock);

    return d;
}

static void
free_machine_desc(struct machine_desc_t *desc) {
    if (!desc) {
        return;
    }

    pthread_mutex_destroy(&desc->lock);
    pfree(desc->pool);
}

// Free the descriptor 
void
machine_destroy_desc(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_descriptors;
    struct machine_desc_t *dp = NULL;

    pthread_mutex_lock(&machine_desc_list_lock);
    while (d) {
        if (d->handle == h) {
            break;
        }
        dp = d;

        d = d->next;
    }

    // handle not found
    if (!d) {
        pthread_mutex_unlock(&machine_desc_list_lock);
        return;

    // handle in first slot
    } else if (!dp) {
        machine_descriptors = d->next;

    } else if (d && dp) {
        dp->next = d->next;
    }
    pthread_mutex_unlock(&machine_desc_list_lock);

    while (d->in_use) {
        usleep(500000);
        continue;
    }

    pthread_mutex_lock(&machine_desc_list_lock);
    free_machine_desc(d);
    pthread_mutex_unlock(&machine_desc_list_lock);
}

void
machine_desc_acquire(struct machine_desc_t *d)
{
    pthread_mutex_lock(&d->lock);
    d->in_use++;
    pthread_mutex_unlock(&d->lock);
}

void
machine_desc_release(struct machine_desc_t *d)
{
    pthread_mutex_lock(&d->lock);
    d->in_use--;
    pthread_mutex_unlock(&d->lock);
}

void
machine_lock(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        return;
    }
    pthread_mutex_lock(&d->lock);
}

void
machine_unlock(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        return;
    }
    pthread_mutex_unlock(&d->lock);
}

/*
 * Return a pointer to the first read descriptor
 */
struct io_desc *
machine_get_read_desc(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        return NULL;
    }
    return d->io_read;
}

/*
 * Return a pointer to the first write descriptor
 */
struct io_desc *
machine_get_write_desc(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        return NULL;
    }
    return d->io_write;
}

void
io_desc_set_state(struct machine_desc_t *d, struct io_desc *io, enum io_desc_state_e new_state)
{
    if (new_state > IO_DESC_ERROR) {
        error("Invalid State (%d)", new_state);
        new_state = IO_DESC_ERROR;
    }

    pthread_mutex_lock(&io->lock);
    enum io_desc_state_e old_state = io->state;
    io->state = new_state;
    pthread_mutex_unlock(&io->lock);

    machine_trace(d, io->handle, "state change from %s to %s",
        IO_DESC_STATE_PRINT(old_state), IO_DESC_STATE_PRINT(new_state));
}

int
machine_desc_read(IO_HANDLE h, void *buf, size_t *bytes)
{
    int ret = IO_ERROR;

    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
        goto do_return;
    }

    if (!d->io_read) {
        machine_error(d, h, "io read descriptor not found");
        goto do_return;
    }

    if (!d->io_read->obj) {
        machine_error(d, h, "io read filter object not found");
        goto do_return;
    }

    switch (d->io_read->state) {
    case IO_DESC_DISABLING:
        machine_warn(d, h, "Disabled: Ignoring read requests until re-enabled");
        io_desc_set_state(d, d->io_read, IO_DESC_DISABLED);
        *bytes = 0;
        ret = IO_SUCCESS;
        goto do_return;

    case IO_DESC_DISABLED:
        *bytes = 0;
        ret = IO_SUCCESS;
        goto do_return;

    case IO_DESC_STOPPED:
        *bytes = 0;
        ret = IO_COMPLETE;
        goto do_return;
    }

    machine_desc_acquire(d);
    struct io_filter_t *f = (struct io_filter_t *)d->io_read->obj;
    ret = f->call(f, buf, bytes, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    machine_desc_release(d);

do_return:
    return ret;
}

int
machine_desc_write(IO_HANDLE h, void *buf, size_t *bytes)
{
    int ret = IO_ERROR;

    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
        goto do_return;
    }

    if (!d->io_write) {
        machine_error(d, h, "io write descriptor not found");
        goto do_return;
    }

    if (!d->io_write->obj) {
        machine_error(d, h, "io write filter object not found");
        goto do_return;
    }

    switch (d->io_write->state) {
    case IO_DESC_DISABLING:
        machine_warn(d, h, "Disabled: Ignoring write requests until re-enabled");
        io_desc_set_state(d, d->io_write, IO_DESC_DISABLED);
        *bytes = 0;
        ret = IO_SUCCESS;
        goto do_return;

    case IO_DESC_DISABLED:
        *bytes = 0;
        ret = IO_SUCCESS;
        goto do_return;

    case IO_DESC_STOPPED:
        *bytes = 0;
        ret = IO_COMPLETE;
        goto do_return;
    }

    machine_desc_acquire(d);
    struct io_filter_t *f = (struct io_filter_t *)d->io_write->obj;
    ret = f->call(f, buf, bytes, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    machine_desc_release(d);

do_return:
    return ret;
}

void
machine_disable_read(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
    } else if (!d->io_write) {
        machine_error(d, h, "io read descriptor not found");
    } else {
        io_desc_set_state(d, d->io_read, IO_DESC_DISABLING);
    }
}

void
machine_disable_write(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
    } else if (!d->io_write) {
        machine_error(d, h, "io write descriptor not found");
    } else {
        io_desc_set_state(d, d->io_write, IO_DESC_DISABLING);
    }
}

void
machine_stop(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
        return;
    }

    if (d->io_write) {
        io_desc_set_state(d, d->io_write, IO_DESC_DISABLING);
    }

    if (d->io_read) {
        io_desc_set_state(d, d->io_read, IO_DESC_DISABLING);
    }
}

IO_HANDLE
machine_desc_init(POOL *p, IOM *machine, IO_DESC *d)
{
    pthread_mutex_init(&d->lock, NULL);
    d->pool = p;

    d->io_read = (struct io_desc *)pcalloc(p, sizeof(struct io_desc));
    if (!d->io_read) {
        printf("Failed to initialize read descriptor\n");
        return IO_ERROR;
    }
    d->io_read->alloc = p;
    pthread_mutex_init(&d->io_read->lock, NULL);

    d->io_write = (struct io_desc *)pcalloc(p, sizeof(struct io_desc));
    if (!d->io_write) {
        printf("Failed to initialize write descriptor\n");
        return IO_ERROR;
    }
    d->io_write->alloc = p;
    pthread_mutex_init(&d->io_write->lock, NULL);

    // Initialize handle to 0 (it's set by machine_register_desc()
    d->handle = 0;
    d->machine = machine;

    return IO_SUCCESS;
}

size_t
machine_get_bytes(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
    }

    error("get_bytes() not implemented for %s: returning 0", d->machine->name);

    return 0;
}

void
machine_set_log_level(char *level)
{
    machine_mgmt_set_log_level(level);
    bw_set_log_level_str(level);
}
