#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <libgen.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>

#include <sys/stat.h>
#include <sys/time.h>

#include "machine.h"
#include "fifo-machine.h"
#include "simple-machines.h"
#include "filter.h"

#define FIFO_IOM_MAX_STRLEN 2048

const IOM *fifo_machine;
static IOM *_fifo_machine = NULL;

struct fifo_desc_t {
    struct machine_desc_t _d;

    // File Name
    char *fname;
    uint32_t flags;

    // File
    FILE *fr;
    FILE *fw;
};

static inline void
set_input_str(POOL *p, char *src, char **dst)
{
    size_t len = strlen(src) + 1;
    len = (len > FIFO_IOM_MAX_STRLEN) ? FIFO_IOM_MAX_STRLEN : len;

    char *str = *dst;

    if (!str) {
        str = pcalloc(p, len);

    } else if (len > strlen(str)) {
        str = repalloc(str, len, p);
    }

    snprintf(str, len, "%s", src);
    *dst = str;
}

static int
open_fifo(struct fifo_desc_t *fd, uint32_t rw)
{
    // Select fifo descriptor
    FILE *f = NULL;
    switch (rw) {
    case FFIFO_READ:
        f = fd->fr;
        break;
    case FFIFO_WRITE:
        f = fd->fw;
        break;
    default:
        return 1;
    }

    // If fifo is already open, return success
    if (f) {
        return 0;
    }

    // Open write fifo
    if (FFIFO_WRITE == rw) {
        // Close open read fifo
        if (fd->fr) {
            fclose(fd->fr);
        }

        f = fd->fw = fopen(fd->fname, "w");
    }

    // Open read fifo
    if (FFIFO_READ == rw) {
        // Close open write fifo
        if (fd->fw) {
            fclose(fd->fw);
        }

        f = fd->fr = fopen(fd->fname, "r");
    }

    if (!f) {
        printf("ERROR: Failed to open fifo %s: %s\n", fd->fname, strerror(errno));
        return 1;
    }

    return 0;
}

static int
fifo_write(IO_FILTER_ARGS)
{
    if (*IO_FILTER_ARGS_BYTES == 0) {
        return IO_SUCCESS;
    }

    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get descriptor from handle
    struct fifo_desc_t *fd = (struct fifo_desc_t *)machine_get_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    if (!(fd->flags & FFIFO_WRITE)) {
        printf("File IOM %d is not set to \"write\" mode\n", *handle);

        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Lock for fifo management
    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);

    // Check if fifo is open
    if (open_fifo(fd, FFIFO_WRITE) == 1) {
        pthread_mutex_unlock(lock);
        return IO_ERROR;
    }
    
    size_t remaining = *IO_FILTER_ARGS_BYTES;
    remaining -= remaining % IO_FILTER_ARGS_ALIGN;
    size_t total = 0;
    char *ptr = IO_FILTER_ARGS_BUF;

    while (remaining) {
        size_t b = fwrite(ptr, 1, remaining, fd->fw);
        remaining -= b;
        ptr += b;
        total += b;
    }
    fflush(fd->fw);

    pthread_mutex_unlock(lock);

    *IO_FILTER_ARGS_BYTES = total;
    return IO_SUCCESS;
}

static int
fifo_read(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get descriptor from handle
    struct fifo_desc_t *fd = (struct fifo_desc_t *)machine_get_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    if (!(fd->flags & FFIFO_READ)) {
        printf("File IOM %d is not set to \"read\" mode\n", *handle);

        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Lock for fifo management
    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);

    // Check if fifo is open
    if (open_fifo(fd, FFIFO_READ) == 1) {
        pthread_mutex_unlock(lock);
        return IO_ERROR;
    }
 
    size_t remaining = *IO_FILTER_ARGS_BYTES;
    remaining -= remaining % IO_FILTER_ARGS_ALIGN;

    size_t total = 0;
    char *ptr = IO_FILTER_ARGS_BUF;

    while (remaining) {
        size_t b = fread(ptr, 1, remaining, fd->fr);
        if (b == 0) {
            int ret = IO_ERROR;
            int eof = feof(fd->fr);
            int err = ferror(fd->fr);
            if (eof) {
                *IO_FILTER_ARGS_BYTES = total;
                ret = IO_COMPLETE;
            } else if (err) {
                printf("File Error: %d\n", err);
            }
            pthread_mutex_unlock(lock);
            return ret;
        }

        remaining -= b;
        ptr += b;
        total += b;
    }

    pthread_mutex_unlock(lock);
    *IO_FILTER_ARGS_BYTES = total;
    return IO_SUCCESS;
}

static IO_HANDLE
create_fifo(void *arg)
{
    struct fifoiom_args *args = (struct fifoiom_args *)arg;

    POOL *p = create_subpool(_fifo_machine->alloc);
    if (!p) {
        printf("ERROR: Failed to create memory pool\n");
        return 0;
    }

    struct fifo_desc_t *desc = pcalloc(p, sizeof(struct fifo_desc_t));
    struct machine_desc_t *d = (struct machine_desc_t *)desc;

    if (!desc) {
        printf("ERROR: Failed to allocate %#zx bytes for fifo descriptor\n", sizeof(struct fifo_desc_t));
        free_pool(p);
        return 0;
    }

    pthread_mutex_init(&d->lock, NULL);
    d->pool = p;

    set_input_str(p, args->fname, &desc->fname);
    desc->flags = args->flags;

    if (machine_desc_init(p, _fifo_machine, (IO_DESC *)desc) < IO_SUCCESS) {
        free_pool(p);
        return 0;
    }

    if (!filter_read_init(p, "_fifo", fifo_read, (IO_DESC *)desc)) {
        printf("ERROR: Failed to initialize read filter\n");
        free_pool(p);
        return 0;
    }

    if (!filter_write_init(p, "_fifo", fifo_write, (IO_DESC *)desc)) {
        printf("ERROR: Failed to initialize write filter\n");
        free_pool(p);
        return 0;
    }

    IO_HANDLE h;
    machine_register_desc((IO_DESC *)desc, &h);

    return h;
}

static void
destroy_fifo(IO_HANDLE h)
{
    struct fifo_desc_t *desc = (struct fifo_desc_t *)machine_get_desc(h);

    if (desc->flags & FFIFO_LEAVE_OPEN == 0) {
        if (desc->fr) {
            fclose(desc->fr);
        }

        if (desc->fw) {
            fclose(desc->fw);
        }
    }

    machine_destroy_desc(h);
}

const IOM*
get_fifo_machine()
{
    IOM *machine = _fifo_machine;
    if (!machine) {
        machine = machine_register("fifo");

        machine->create = create_fifo;
        machine->destroy = destroy_fifo;
        machine->stop = machine_disable_read;
        machine->read = machine_desc_read;
        machine->write = machine_desc_write;
        machine->obj = NULL;

        _fifo_machine = machine;
        fifo_machine = machine;
    }
    return (const IOM *)machine;
}

void
fifo_iom_set_leave_open(IO_HANDLE h)
{
    struct fifo_desc_t *fd = (struct fifo_desc_t *)machine_get_desc(h);
    fd->flags |= FFIFO_LEAVE_OPEN;
}

IO_HANDLE
new_fifo_machine(char *fname, uint32_t flags)
{
    struct fifoiom_args args;
    memset(&args, 0, sizeof(struct fifoiom_args));

    args.fname = fname;
    args.flags = flags;

    const IOM *fm = get_fifo_machine();
    return fm->create(&args);
}

IO_HANDLE
new_fifo_read_machine(char *fname)
{
    return new_fifo_machine(fname, FFIFO_READ);
}

IO_HANDLE
new_fifo_write_machine(char *fname)
{
    return new_fifo_machine(fname, FFIFO_WRITE);
}
