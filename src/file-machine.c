#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <radpool.h>

#include "machine.h"
#include "simple-machines.h"
#include "filter.h"

#define MAX_FILEPATH_LEN 2048

static IOM *file_machine = NULL;

enum file_mode_e {
    F_NOMODE,
    F_READ,
    F_WRITE,
    F_WRITE_ROTATE,
};

enum file_rotate_e {
    F_NOROTATE=0,
    F_ROTATE,
};

struct file_desc_t {
    struct machine_desc_t _d;
    char *fname;
    FILE *f;
    enum file_mode_e mode;
    enum file_rotate_e rotate;
    uint16_t rotate_index;
    char binary;
    size_t rp;
    size_t wp;
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
    case F_WRITE_ROTATE:
     {
        // Add the file index as a 5-digit number (index is 2-bytes)
        char fname[MAX_FILEPATH_LEN];
        snprintf(fname, MAX_FILEPATH_LEN, "%s.%05d", fd->fname, fd->rotate_index++);
        fname[MAX_FILEPATH_LEN - 1] = '\0';
        if (fd->binary) {
            fd->f = fopen(fname, "wb");
        } else {
            fd->f = fopen(fname, "w");
        }
        return;
     }

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
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    pthread_mutex_t *lock = &fd->_d.lock;

    pthread_mutex_lock(lock);
    switch (fd->mode) {
    case F_NOMODE:
        if (fd->rotate) {
            fd->mode = F_WRITE_ROTATE;
        } else {
            fd->mode = F_WRITE;
        }
        open_file(fd);
        break;

    case F_WRITE_ROTATE:
        fclose(fd->f);
        fd->f = 0;
        open_file(fd);
        break;

    case F_WRITE:
        break;

    case F_READ:
        fclose(fd->f);
        fd->f = NULL;
        if (fd->rotate) {
            fd->mode = F_WRITE_ROTATE;
        } else {
            fd->mode = F_WRITE;
        }
        open_file(fd);
        break;
    default:
        *IO_FILTER_ARGS_BYTES = 0;
        pthread_mutex_unlock(lock);
        return IO_ERROR;
    }
    
    size_t remaining = *IO_FILTER_ARGS_BYTES;
    remaining -= remaining % IO_FILTER_ARGS_ALIGN;
    size_t total = 0;
    char *ptr = IO_FILTER_ARGS_BUF;

    while (remaining) {
        size_t b = fwrite(ptr, 1, remaining, fd->f);
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
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(*handle);
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
    case F_WRITE_ROTATE:
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

    size_t remaining = *IO_FILTER_ARGS_BYTES;
    remaining -= remaining % IO_FILTER_ARGS_ALIGN;

    size_t total = 0;
    char *ptr = IO_FILTER_ARGS_BUF;

    while (remaining) {
        size_t b = fread(ptr, 1, remaining, fd->f);
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

static char *
build_filepath_str(POOL *p, struct fileiom_args *args) {
    uint32_t pathlen = 0;

    // Validate directory len
    if (args->outdir) {
        pathlen = strlen(args->outdir);

        // Don't forget the path separator
        if (args->outdir[pathlen] != '/') {
            pathlen++;
        }
    }

    // Rotating files add 6 characters: ".00000"
    if (F_ROTATE == args->is_rotate) {
        pathlen += 6;
    }

    pathlen += strlen(args->fname);

    if (pathlen >= MAX_FILEPATH_LEN) {
        printf("ERROR: File path too long\n");
        return NULL;
    }

    // Using stpcpy instead of strncpy.  Size calculation was completed above.
    char *fname = (char *)pcalloc(p, MAX_FILEPATH_LEN);
    char *ptr = fname;
    if (args->outdir) {
        ptr = stpcpy(ptr, args->outdir);

        // Don't forget the path separator
        // stpcpy returns a pointer to the null-terminator.  The -1 index is the
        // last character of the string.
        if (ptr[-1] != '/') {
            ptr = stpcpy(ptr, "/");
        }
    }

    // Calcluate how much buffer space is remaining
    uint32_t remaining = MAX_FILEPATH_LEN - (uint32_t)(ptr - fname);
    strncpy(ptr, args->fname, remaining);

    // Better safe than sorry
    fname[MAX_FILEPATH_LEN - 1] = '\0';

    return fname;
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

    desc->fname = build_filepath_str(p, args);
    if (!desc->fname) {
        pfree(p);
        return 0;
    }

    desc->binary = args->is_binary;
    desc->rotate = args->is_rotate;
    desc->rp = 0;
    desc->wp = 0;

    if (machine_desc_init(p, file_machine, (IO_DESC *)desc) != IO_SUCCESS) {
        pfree(p);
        return 0;
    }

    if (!filter_read_init(p, "_file", file_read, (IO_DESC *)desc)) {
        printf("ERROR: Failed to initialize read filter\n");
        pfree(p);
        return 0;
    }

    if (!filter_write_init(p, "_file", file_write, (IO_DESC *)desc)) {
        printf("ERROR: Failed to initialize write filter\n");
        pfree(p);
        return 0;
    }

    IO_HANDLE h;
    machine_register_desc((IO_DESC *)desc, &h);

    return h;
}

static void
destroy_file(IO_HANDLE h)
{
    struct file_desc_t *desc = (struct file_desc_t *)machine_get_desc(h);
    fclose(desc->f);
    machine_destroy_desc(h);
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
        machine->read = machine_desc_read;
        machine->write = machine_desc_write;
        machine->obj = NULL;

        file_machine = machine;
    }
    return (const IOM *)machine;
}

IO_HANDLE
new_file_machine(char *fname, char *outdir, enum filetype_e type)
{
    const IOM *file_machine = get_file_machine();
    struct fileiom_args args = {fname, outdir, type, F_NOROTATE};
    return file_machine->create(&args);
}

IO_HANDLE
new_rotating_file_machine(char *fname, char *outdir, enum filetype_e type)
{
    const IOM *file_machine = get_file_machine();
    struct fileiom_args args = {fname, outdir, type, F_ROTATE};
    return file_machine->create(&args);
}
