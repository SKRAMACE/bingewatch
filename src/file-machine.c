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

#define FILE_IOM_MAX_STRLEN 1024
#define FILE_IOM_MAX_PATHLEN 2048
#define TIMESTAMP_FMT "%Y/%m/%d/%H"

const IOM *file_machine;
static IOM *_file_machine = NULL;

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
set_input_str(POOL *p, char *src, char **dst)
{
    size_t len = strlen(src) + 1;
    len = (len > FILE_IOM_MAX_STRLEN) ? FILE_IOM_MAX_STRLEN : len;

    char *str = *dst;

    if (!str) {
        str = pcalloc(p, len);

    } else if (len > strlen(str)) {
        str = repalloc(str, len, p);
    }

    snprintf(str, len, "%s", src);
    *dst = str;
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
    if (stat(dirname, &s) == 0 && S_ISDIR(s.st_mode)) {
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
rotate_wfile(struct file_desc_t *fd)
{
    // Close write file
    if (fd->fw) {
        fclose(fd->fw);
        fd->fw = NULL;
    }

    if (fd->flags & FFILE_INDEX) {
        fd->file_index++;
    }
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

static void
build_directory_path(struct file_desc_t *fd, char *path, size_t *bytes)
{
    char *p = path;
    size_t r = *bytes;

    // Start with root dir
    int x = snprintf(p, r, "%s", fd->root_dir);
    p += x;
    r -= x;

    if (fd->flags & FFILE_AUTO_DATE) {
        char timestamp[32];
        gen_timestamp(timestamp, 32, fd->date_fmt);

        if (strncmp(timestamp, fd->timestamp, 32) != 0) {
            strncpy(fd->timestamp, timestamp, 32);
            fd->basedir_index = 0;
            fd->file_index = 0;
        }

        x = snprintf(p, r, "/%s", timestamp);
        p += x;
        r -= x;
    }

    if (fd->flags & FFILE_DIR_ROTATE) {
        if (fd->base_dir) {
            x = snprintf(p, r, "/%s-%05d",
                fd->base_dir, fd->basedir_index);
        } else {
            x = snprintf(p, r, "/%05d", fd->basedir_index);
        }
        p += x;
        r -= x;
    }

    *bytes = (size_t)(p - path);
}

static int
open_file(struct file_desc_t *fd, uint32_t rw)
{
    // Select file descriptor
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

    // If file is already open, return success
    if (f) {
        return 0;
    }

    // Try to create directory
    char path[FILE_IOM_MAX_PATHLEN];
    size_t len = FILE_IOM_MAX_PATHLEN;
    build_directory_path(fd, path, &len);
    if (create_dir(path) != 0) {
        printf("ERROR: Failed to create directory \"%s\"\n", path);
        return 1;
    }

    char fname[FILE_IOM_MAX_PATHLEN];

    // Open write file
    if (FFILE_WRITE == rw) {
        // Close open read file
        if (fd->fr) {
            fclose(fd->fr);
        }

        if (fd->flags & FFILE_INDEX) {
            snprintf(fname, FILE_IOM_MAX_PATHLEN, "%s/%s-%05d%s%s",
                path, fd->file_tag, fd->file_index, 
                (fd->file_ext) ? "." : "", (fd->file_ext) ? fd->file_ext : "");
        } else {
            snprintf(fname, FILE_IOM_MAX_PATHLEN, "%s/%s%s%s",
                path, fd->file_tag,
                (fd->file_ext) ? "." : "", (fd->file_ext) ? fd->file_ext : "");
        }

        struct stat s;
        if ((fd->flags & FFILE_ROTATE) && stat(fname, &s) == 0) {
            uint32_t index = 1;
            while (stat(fname, &s) == 0) {
                snprintf(fname, FILE_IOM_MAX_PATHLEN, "%s/%s-%05d%s%s",
                    path, fd->file_tag, index++, 
                    (fd->file_ext) ? "." : "", (fd->file_ext) ? fd->file_ext : "");
            }

            if (fd->flags & FFILE_INDEX) {
                fd->file_index = index;
            }
        }

        f = fd->fw = fopen(fname, "w");
    }

    // Open read file
    if (FFILE_READ == rw) {
        // Close open write file
        if (fd->fw) {
            fclose(fd->fw);
        }

        snprintf(fname, FILE_IOM_MAX_PATHLEN, "%s/%s%s%s",
            path, fd->file_tag,
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

    if (fd->flags & FFILE_ROTATE) {
        rotate_wfile(fd);
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

    POOL *p = create_subpool(_file_machine->alloc);
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

    set_input_str(p, args->fname, &desc->file_tag);

    if (args->root_dir) {
        set_input_str(p, args->root_dir, &desc->root_dir);
    }

    if (args->base_dir) {
        set_input_str(p, args->base_dir, &desc->base_dir);
    }

    if (args->ext) {
        set_input_str(p, args->ext, &desc->file_ext);
    }

    desc->basedir_index = 0;
    desc->file_index = 0;
    desc->date_fmt = TIMESTAMP_FMT;

    desc->flags = args->flags;

    if (machine_desc_init(p, _file_machine, (IO_DESC *)desc) != IO_SUCCESS) {
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
    rotate_wfile(fd);
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
    IOM *machine = _file_machine;
    if (!machine) {
        machine = machine_register("file");

        machine->create = create_file;
        machine->destroy = destroy_file;
        machine->stop = machine_disable_read;
        machine->read = machine_desc_read;
        machine->write = machine_desc_write;
        machine->obj = NULL;

        _file_machine = machine;
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
     
    fd->flags |= (FFILE_ROTATE | FFILE_INDEX);
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

void
file_iom_set_filetag(IO_HANDLE h, char *file_tag)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return;
    }

    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);

    rotate_wfile(fd);
    set_input_str(fd->_d.pool, file_tag, &fd->file_tag);
    fd->file_index = 0;

    pthread_mutex_unlock(lock);
}

void
file_iom_set_rootdir(IO_HANDLE h, char *root_dir)
{
    struct file_desc_t *fd = (struct file_desc_t *)machine_get_desc(h);
    if (!fd) {
        return;
    }

    pthread_mutex_t *lock = &fd->_d.lock;
    pthread_mutex_lock(lock);

    set_input_str(fd->_d.pool, root_dir, &fd->root_dir);

    pthread_mutex_unlock(lock);
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

    const IOM *fm = get_file_machine();
    return fm->create(&args);
}

IO_HANDLE
new_file_read_machine(char *fname)
{
    char dirstr[FILE_IOM_MAX_PATHLEN];
    int len = snprintf(dirstr, FILE_IOM_MAX_PATHLEN, "%s", fname);
    if (len < 0) {
        printf("Error formatting filename: %s\n", strerror(errno));
        return 0;
    }

    if (len > FILE_IOM_MAX_PATHLEN) {
        printf("Filename exceeded max length (%d > %d)\n", len, FILE_IOM_MAX_PATHLEN);
        return 0;
    }

    char basestr[FILE_IOM_MAX_PATHLEN];
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

    POOL *p = create_subpool(_file_machine->alloc);

    char name[64];
    snprintf(name, 64, "file-machine-%d rotate filter", h);
    IO_FILTER *f = create_filter(p, name, file_rotate_fn);

    fd->flags |= FFILE_INDEX;
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

    POOL *p = create_subpool(_file_machine->alloc);

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
