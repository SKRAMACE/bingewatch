#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <radpool.h>

#include "machine.h"
#include "simple-machines.h"
#include "filter.h"

static IOM *file_machine = NULL;

enum file_mode_e {
    F_NOMODE,
    F_READ,
    F_WRITE,
};

struct file_desc_t {
    struct machine_desc_t _d;
    char *fname;
    FILE *f;
    enum file_mode_e mode;
    char binary;
    uint64_t rp;
    uint64_t wp;
};

static inline void
sanitize_args(struct fileiom_args *args)
{
}

//TODO: Keep track of file pointer
static void
open_file(struct file_desc_t *fd)
{
    if (fd->f) {
        printf("File is already open\n");
        return;
    }

    switch (fd->mode) {
    case F_WRITE:
        if (fd->binary) {
            fd->f = fopen(fd->fname, "wb");
        } else {
            fd->f = fopen(fd->fname, "w");
        }
        return;

    case F_READ:
        if (fd->binary) {
            fd->f = fopen(fd->fname, "rb");
        } else {
            fd->f = fopen(fd->fname, "r");
        }
        return;
    default:
        return;
    }
}

//TODO: Keep track of file pointer
static int
file_write(IO_FILTER_ARGS)
{
    // TODO: THIS MAY BE SLOW.  LOOK INTO USING A POINTER TO THE ACTUAL DESCRIPTOR
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get socket from handle
    struct file_desc_t *fd = (struct file_desc_t *)get_machine_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    pthread_mutex_t *lock = &fd->_d.lock;

    pthread_mutex_lock(lock);
    switch (fd->mode) {
    case F_NOMODE:
        fd->mode = F_WRITE;
        open_file(fd);
    case F_WRITE:
        break;
    case F_READ:
        fclose(fd->f);
        fd->f = NULL;
        fd->mode = F_WRITE;
        open_file(fd);
        break;
    default:
        *IO_FILTER_ARGS_BYTES = 0;
        pthread_mutex_unlock(lock);
        return IO_ERROR;
    }
    
    uint64_t remaining = *IO_FILTER_ARGS_BYTES;
    remaining -= remaining % IO_FILTER_ARGS_ALIGN;
    uint64_t total = 0;
    char *ptr = IO_FILTER_ARGS_BUF;

    while (remaining) {
        uint64_t b = fwrite(ptr, 1, remaining, fd->f);
        remaining -= b;
        ptr += b;
        total += b;
    }
    pthread_mutex_unlock(lock);

    *IO_FILTER_ARGS_BYTES = total;
    return IO_SUCCESS;
}

static int
file_read(IO_FILTER_ARGS)
{
    // TODO: THIS MAY BE SLOW.  LOOK INTO USING A POINTER TO THE ACTUAL DESCRIPTOR
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get socket from handle
    struct file_desc_t *fd = (struct file_desc_t *)get_machine_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    pthread_mutex_t *lock = &fd->_d.lock;

    pthread_mutex_lock(lock);
    switch (fd->mode) {
    case F_NOMODE:
        fd->mode = F_READ;
        open_file(fd);
    case F_READ:
        break;
    case F_WRITE:
        fclose(fd->f);
        fd->f = NULL;
        fd->mode = F_READ;
        open_file(fd);
        break;
    default:
        *IO_FILTER_ARGS_BYTES = 0;
        pthread_mutex_unlock(lock);
        return IO_ERROR;
    }

    if (!fd->f) {
        printf("ERROR: File not found (%s)\n", fd->fname);
        return IO_ERROR;
    }

    uint64_t remaining = *IO_FILTER_ARGS_BYTES;
    remaining -= remaining % IO_FILTER_ARGS_ALIGN;

    uint64_t total = 0;
    char *ptr = IO_FILTER_ARGS_BUF;

    while (remaining) {
        uint64_t b = fread(ptr, 1, remaining, fd->f);
        if (b == 0) {
            int ret = IO_ERROR;
            int eof = feof(fd->f);
            int err = ferror(fd->f);
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

static enum io_status
init_filters(struct file_desc_t *f)
{
    struct machine_desc_t *d = (struct machine_desc_t *)f;
    POOL *pool = d->pool;

    // Create io descriptors
    d->io_read = (struct io_desc *)pcalloc(pool, sizeof(struct io_desc));
    d->io_write = (struct io_desc *)pcalloc(pool, sizeof(struct io_desc));

    d->io_read->alloc = pool;
    d->io_write->alloc = pool;

    // Create base filters
    struct io_filter_t *fil;
    IO_HANDLE *h;

    // Read Filter
    fil = create_filter(pool, "_file", file_read);
    if (!fil) {
        printf("Failed to initialize read filter\n");
        return IO_ERROR;
    }
    h = palloc(fil->alloc, sizeof(IO_HANDLE));
    *h = d->handle;
    fil->obj = h;
    d->io_read->obj = fil;

    // Write Filter
    fil = create_filter(pool, "_file", file_write);
    if (!fil) {
        printf("Failed to initialize read filter\n");
        return IO_ERROR;
    }
    h = palloc(fil->alloc, sizeof(IO_HANDLE));
    *h = d->handle;
    fil->obj = h;
    d->io_write->obj = fil;

    return IO_SUCCESS;
}

static IO_HANDLE
create_file(void *arg)
{
    struct fileiom_args *args = (struct fileiom_args *)arg;
    sanitize_args(args);

    POOL *p = create_subpool(file_machine->alloc);
    if (!p) {
        printf("ERROR: Failed to create memory pool\n");
        return 0;
    }

    struct file_desc_t *desc = pcalloc(p, sizeof(struct file_desc_t));
    struct machine_desc_t *d = (struct machine_desc_t *)desc;

    if (!desc) {
        printf("ERROR: Failed to allocate %" PRIx64 " bytes for file descriptor\n", sizeof(struct file_desc_t));
        pfree(p);
        return 0;
    }

    pthread_mutex_init(&d->lock, NULL);
    d->pool = p;
    d->handle = request_handle(file_machine);

    char *fname = pcalloc(p, 256);
    strncpy(fname, args->fname, 255);
    fname[255] = '\0';
    desc->fname = fname;
    desc->binary = args->is_binary;
    desc->rp = 0;
    desc->wp = 0;

    int status = init_filters(desc);
    if (status != IO_SUCCESS) {
        printf("ERROR: Failed to initialize filters\n");
        pfree(p);
        return 0;
    }

    add_machine_desc(d);
    return d->handle;
}

static void
destroy_file(IO_HANDLE h)
{
    struct file_desc_t *desc = (struct file_desc_t *)get_machine_desc(h);
    fclose(desc->f);
    destroy_machine_desc(h);
}

const IOM*
get_file_machine()
{
    IOM *machine = file_machine;
    if (!machine) {
        machine = machine_register("file");

        machine->create = create_file;
        machine->destroy = destroy_file;
        machine->stop = machine_disable_read;
        machine->lock = machine_desc_lock;
        machine->unlock = machine_desc_unlock;
        machine->get_read_desc = get_read_desc;
        machine->get_write_desc = get_write_desc;
        machine->read = machine_desc_read;
        machine->write = machine_desc_write;
        machine->obj = NULL;

        file_machine = machine;
    }
    return (const IOM *)machine;
}
