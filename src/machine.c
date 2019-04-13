#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <radpool.h>

#include "machine.h"
#include "filter.h"

struct machine_desc_t *machine_descriptors = NULL;
static pthread_mutex_t machine_desc_list_lock = PTHREAD_MUTEX_INITIALIZER;

static inline void
acquire_machine_desc(struct machine_desc_t *d)
{
    pthread_mutex_lock(&d->lock);
    d->in_use++;
    pthread_mutex_unlock(&d->lock);
}

static inline void
release_machine_desc(struct machine_desc_t *d)
{
    pthread_mutex_lock(&d->lock);
    d->in_use--;
    pthread_mutex_unlock(&d->lock);
}

// Add a new descriptor
void
add_machine_desc(struct machine_desc_t *desc)
{
    pthread_mutex_lock(&machine_desc_list_lock);
    struct machine_desc_t *d = machine_descriptors;
    if (!d) {
        machine_descriptors = desc;
    } else {
        while (d->next) {
            d = d->next;
        }
        d->next = desc;
    }
    pthread_mutex_unlock(&machine_desc_list_lock);
}

// Get a pointer to a descriptor with the specified handle
struct machine_desc_t *
get_machine_desc(IO_HANDLE h)
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
destroy_machine_desc(IO_HANDLE h)
{
    pthread_mutex_lock(&machine_desc_list_lock);
    struct machine_desc_t *d = machine_descriptors;

    struct machine_desc_t *dp = NULL;
    while (d) {
        if (d->handle == h) {
            break;
        }
        dp = d;

        d = d->next;
    }

    // handle not found
    if (!d) {
        return;

    // handle in first slot
    } else if (!dp) {
        machine_descriptors = d->next;

    } else if (d && dp) {
        dp->next = d->next;
    }
    pthread_mutex_unlock(&machine_desc_list_lock);

    while (d->in_use) {
        continue;
    }

    pthread_mutex_lock(&machine_desc_list_lock);
    free_machine_desc(d);
    pthread_mutex_unlock(&machine_desc_list_lock);
}

void
machine_desc_lock(IO_HANDLE h)
{
    struct machine_desc_t *d = get_machine_desc(h);
    pthread_mutex_lock(&d->lock);
}

void
machine_desc_unlock(IO_HANDLE h)
{
    struct machine_desc_t *d = get_machine_desc(h);
    pthread_mutex_unlock(&d->lock);
}

struct io_desc *
get_read_desc(IO_HANDLE h)
{
    struct machine_desc_t *d = get_machine_desc(h);
    return d->io_read;
}

struct io_desc *
get_write_desc(IO_HANDLE h)
{
    struct machine_desc_t *d = get_machine_desc(h);
    return d->io_write;
}

int
machine_desc_read(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct machine_desc_t *d = get_machine_desc(h);
    if (!d) {
        *len = 0;
        return IO_ERROR;
    }

    if (d->io_read->disabled) {
        *len = 0;
        return IO_COMPLETE;
    }

    acquire_machine_desc(d);
    struct io_filter_t *f = (struct io_filter_t *)d->io_read->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_machine_desc(d);

    return status;
}

int
machine_desc_write(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct machine_desc_t *d = get_machine_desc(h);
    if (!d) {
        *len = 0;
        return IO_ERROR;
    }

    if (d->io_write->disabled) {
        *len = 0;
        return IO_COMPLETE;
    }

    acquire_machine_desc(d);
    struct io_filter_t *f = (struct io_filter_t *)d->io_write->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_machine_desc(d);

    return status;
}

void
machine_disable_read(IO_HANDLE h)
{
    struct machine_desc_t *d = get_machine_desc(h);
    if (d) {
        d->io_read->disabled = 1;
    }
    
}

void
machine_disable_write(IO_HANDLE h)
{
    struct machine_desc_t *d = get_machine_desc(h);
    if (d) {
        d->io_write->disabled = 1;
    }
    
}

