#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <libgen.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <radpool.h>

#include <sys/stat.h>
#include <sys/time.h>

#include "machine.h"
#include "simple-machines.h"
#include "filter.h"

#define MAX_FILEPATH_LEN 2048
#define TIMESTAMP_FMT "%Y/%m/%d/%H"

static IOM *file_machine = NULL;

enum file_rotate_e {
    F_NOROTATE=0,
    F_ROTATE,
};

struct file_desc_t {
    struct machine_desc_t _d;

    // File Name
    char *root_dir;
    char *base_dir;
    char *file_tag;
    char *file_ext;
    char timestamp[32];
    const char *date_fmt;

    uint32_t flags;
    uint32_t basedir_index;
    uint32_t file_index;

    // File
    FILE *fr;
    FILE *fw;
};

static inline void
sanitize_args(struct fileiom_args *args)
{
}

static void
gen_timestamp_fmt(char *timestamp, size_t bytes, const char *fmt)
{
    // Get time with microsecond precision
    struct timeval tv;
    gettimeofday(&tv, NULL);

    // Convert seconds to "struct tm" (compatible with strftime)
    time_t nowtime = tv.tv_sec;
    struct tm *nowtm = gmtime(&nowtime);

    char timestr[64];
    strftime(timestr, 64, fmt, nowtm);

    snprintf(timestamp, bytes, "%s", timestr);
}

static void
gen_timestamp(char *timestamp, size_t bytes, const char *fmt)
{
    gen_timestamp_fmt(timestamp, bytes, fmt);
}

static int
create_dir(char *dirname)
{
    struct stat s;
    stat(dirname, &s);

    if (S_ISDIR(s.st_mode)) {
        return 0;
    }

    if (mkdir(dirname, 0755) == 0) {
        return 0;
    }

    size_t len = strlen(dirname) + 1;
    char *dir = malloc(len);
    snprintf(dir, len, "%s", dirname);

    // Handle trailing "/"
    if (dir[len - 1] == '/') {
        dir[len - 1] = 0;
    }

    char *base = strrchr(dir, '/');
    if (!base || base == dir) {
        printf("ERROR: Failed to create directory %s\n", dir);
        return 1;
    }

    *base = 0;
    if (create_dir(dir) != 0) {
        printf("ERROR: Failed to create directory %s\n", dir);
        return 1;
    }
    free(dir);

    if (mkdir(dirname, 0755) != 0) {
        printf("ERROR: Failed to create directory %s\n", dirname);
        return 1;
    }

    return 0;
}

static void
rotate_file(struct file_desc_t *fd)
{
    if (fd->fw) {
        fclose(fd->fw);
        fd->fw = NULL;
    }

    fd->file_index++;
}

static void
rotate_basedir(struct file_desc_t *fd)
{
    if (fd->fw) {
        fclose(fd->fw);
        fd->fw = NULL;
    }

    fd->file_index = 0;
    fd->basedir_index++;
}

static int
open_file(struct file_desc_t *fd, uint32_t rw)
{
    FILE *f = NULL;
    switch (rw) {
    case FFILE_READ:
        f = fd->fr;
        break;
    case FFILE_WRITE:
        f = fd->fw;
        break;
    default:
        return 1;
    }

    if (f) {
        return 0;
    }

    char dirname[MAX_FILEPATH_LEN];
    int off = 0;

    int x = snprintf(dirname + off, MAX_FILEPATH_LEN - off, "%s", fd->root_dir);
    off += x;

    if (fd->flags & FFILE_AUTO_DATE) {
        char timestamp[32];
        gen_timestamp(timestamp, 32, fd->date_fmt);

        if (strncmp(timestamp, fd->timestamp, 32) != 0) {
            strncpy(fd->timestamp, timestamp, 32);
            fd->basedir_index = 0;
            fd->file_index = 0;
        }

        x = snprintf(dirname + off, MAX_FILEPATH_LEN - off, "/%s", timestamp);
        off += x;
    }

    if (fd->flags & FFILE_DIR_ROTATE) {
        if (fd->base_dir) {
            x = snprintf(dirname + off, MAX_FILEPATH_LEN - off, "/%s-%05d",
                fd->base_dir, fd->basedir_index);
        } else {
            x = snprintf(dirname + off, MAX_FILEPATH_LEN - off, "/%05d", fd->basedir_index);
        }
        off += x;
    }

    if (create_dir(dirname) != 0) {
        return 1;
    }

    char fname[MAX_FILEPATH_LEN];

    if (FFILE_WRITE == rw) {
        if (fd->fr) {
            fclose(fd->fr);
        }

        if (fd->flags & FFILE_ROTATE) {
            snprintf(fname, MAX_FILEPATH_LEN, "%s/%s-%05d%s%s",
                dirname, fd->file_tag, fd->file_index, 
                (fd->file_ext) ? "." : "", (fd->file_ext) ? fd->file_ext : "");
        } else {
            snprintf(fname, MAX_FILEPATH_LEN, "%s/%s%s%s",
                dirname, fd->file_tag,
                (fd->file_ext) ? "." : "", (fd->file_ext) ? fd->file_ext : "");
        }
        f = fd->fw = fopen(fname, "w");
    }

    if (FFILE_READ == rw) {
        if (fd->fw) {
            fclose(fd->fw);
        }

        snprintf(fname, MAX_FILEPATH_LEN, "%s/%s%s%s",
            dirname, fd->file_tag,
            (fd->file_ext) ? "." : "", (fd->file_ext) ? fd->file_ext : "");
        f = fd->fr = fopen(fname, "r");
    }

    if (!f) {
        printf("ERROR: Failed to open file %s: %s\n", fname, strerror(errno));
        return 1;
    }

    return 0;
}

static int
file_write(IO_FILTER_ARGS)
{
    if (*IO_FILTER_ARGS_BYTES == 0) {
        return IO_SUCCESS;
    }

    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get descriptor from handle
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    if (!(fd->flags & FFILE_WRITE)) {
        printf("File IOM %d is not set to \"write\" mode\n", *handle);

        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Lock for file management
    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);

    // Check if file is open
    if (open_file(fd, FFILE_WRITE) == 1) {
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

    if (fd->flags & FFILE_AUTO_ROTATE) {
        rotate_file(fd);
    }
    pthread_mutex_unlock(lock);

    *IO_FILTER_ARGS_BYTES = total;
    return IO_SUCCESS;
}

static int
file_read(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get descriptor from handle
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(*handle);
    if (!fd) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    if (!(fd->flags & FFILE_READ)) {
        printf("File IOM %d is not set to \"read\" mode\n", *handle);

        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Lock for file management
    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);

    // Check if file is open
    if (open_file(fd, FFILE_READ) == 1) {
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
        printf("ERROR: Failed to allocate %#zx bytes for file descriptor\n", sizeof(struct file_desc_t));
        pfree(p);
        return 0;
    }

    pthread_mutex_init(&d->lock, NULL);
    d->pool = p;

    // Validate directory len
    size_t len = strlen(args->fname);
    desc->file_tag = pcalloc(p, len + 1);
    snprintf(desc->file_tag, len, "%s", args->fname);
    strncpy(desc->file_tag, args->fname, len);

    if (args->root_dir) {
        size_t len = strlen(args->root_dir);
        desc->root_dir = pcalloc(p, len + 1);
        strncpy(desc->root_dir, args->root_dir, len);
    }

    if (args->base_dir) {
        size_t len = strlen(args->base_dir);
        desc->base_dir = pcalloc(p, len + 1);
        strncpy(desc->base_dir, args->base_dir, len);
    }

    if (args->ext) {
        size_t len = strlen(args->ext);
        desc->file_ext = pcalloc(p, len + 1);
        strncpy(desc->file_ext, args->ext, len);
    }

    desc->basedir_index = 0;
    desc->file_index = 0;
    desc->date_fmt = TIMESTAMP_FMT;

    desc->flags = args->flags;

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

    if (desc->fr) {
        fclose(desc->fr);
    }

    if (desc->fw) {
        fclose(desc->fw);
    }

    machine_destroy_desc(h);
}

static int
file_rotate_fn(IO_FILTER_ARGS)
{
    int ret = IO_ERROR;

    // Dereference filter variables
    struct file_desc_t *fd = (struct file_desc_t *)IO_FILTER_ARGS_FILTER->obj;

    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);
    rotate_file(fd);
    pthread_mutex_unlock(lock);

    ret = CALL_NEXT_FILTER();
}

static int
dir_rotate_fn(IO_FILTER_ARGS)
{
    int ret = IO_ERROR;

    // Dereference filter variables
    struct file_desc_t *fd = (struct file_desc_t *)IO_FILTER_ARGS_FILTER->obj;

    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);
    rotate_basedir(fd);
    pthread_mutex_unlock(lock);

    ret = CALL_NEXT_FILTER();
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

void
file_iom_set_flags(IO_HANDLE h, uint32_t flags)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return;
    }

    fd->flags = flags;
}

void
file_iom_set_auto_rotate(IO_HANDLE h)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return;
    }
     
    fd->flags |= (FFILE_ROTATE | FFILE_AUTO_ROTATE);
}

void
file_iom_set_auto_date(IO_HANDLE h)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return;
    }
     
    fd->flags |= FFILE_AUTO_DATE;
}

void
file_iom_set_auto_date_fmt(IO_HANDLE h, const char *fmt)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return;
    }
     
    fd->date_fmt = fmt;
}

IO_HANDLE
new_file_machine(char *rootdir, char *fname, char *ext, uint32_t flags)
{
    struct fileiom_args args;
    memset(&args, 0, sizeof(struct fileiom_args));

    args.fname = fname;
    args.root_dir = rootdir;
    args.ext = ext;
    args.flags = flags;

    const IOM *file_machine = get_file_machine();
    return file_machine->create(&args);
}

IO_HANDLE
new_file_read_machine(char *fname)
{
    char dirstr[MAX_FILEPATH_LEN];
    int len = snprintf(dirstr, MAX_FILEPATH_LEN, "%s", fname);
    if (len < 0) {
        printf("Error formatting filename: %s\n", strerror(errno));
        return 0;
    }

    if (len > MAX_FILEPATH_LEN) {
        printf("Filename exceeded max length (%d > %d)\n", len, MAX_FILEPATH_LEN);
        return 0;
    }

    char basestr[MAX_FILEPATH_LEN];
    strncpy(basestr, dirstr, len+1);

    char *root = dirname(dirstr);
    char *base = basename(basestr);

    char *ext = strrchr(base, '.');
    if (!ext || ext == base) {
        ext = NULL;
    } else {
        *ext++ = 0;
    }

    return new_file_machine(root, base, ext, FFILE_READ);
}

IO_HANDLE
new_file_write_machine(char *rootdir, char *fname, char *ext)
{
    return new_file_machine(rootdir, fname, ext, FFILE_WRITE);
}

IO_FILTER *
file_rotate_filter(IO_HANDLE h)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return NULL;
    }

    POOL *p = create_subpool(file_machine->alloc);

    char name[64];
    snprintf(name, 64, "file-machine-%d rotate filter", h);
    IO_FILTER *f = create_filter(p, name, file_rotate_fn);

    fd->flags |= FFILE_ROTATE;
    fd->file_index = 0;

    f->obj = fd;

    return f;
}

IO_FILTER *
file_dir_rotate_filter(IO_HANDLE h, const char *basedir)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return NULL;
    }

    POOL *p = create_subpool(file_machine->alloc);

    char name[64];
    snprintf(name, 64, "file-machine-%d dir rotate filter", h);
    IO_FILTER *f = create_filter(p, name, dir_rotate_fn);

    fd->flags |= (FFILE_ROTATE | FFILE_DIR_ROTATE);
    fd->basedir_index = 0;
    fd->file_index = 0;

    if (basedir) {
        size_t len = strlen(basedir) + 1;
        fd->base_dir = pcalloc(fd->_d.pool, len);
        snprintf(fd->base_dir, len, "%s", basedir);
    }

    f->obj = fd;

    return f;
}
