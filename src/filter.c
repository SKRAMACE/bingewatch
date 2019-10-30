#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "radpool.h"
#include "machine.h"
#include "filter.h"

struct fb_ctl_t {
    char *filter_buf;
    size_t bytes;
};

struct io_filter_t *
create_filter(void *alloc, const char *name, io_filter_fn fn)
{
    POOL *pool = (POOL *)alloc;
    POOL *fpool = create_subpool(pool);

    struct io_filter_t *filter = pcalloc(pool, sizeof(struct io_filter_t));
    filter->direction = IOF_BIDIRECTIONAL;
    filter->enabled = 1;
    filter->alloc = fpool;
    filter->next = NULL;
    filter->call = fn;

    strncpy(filter->name, name, IO_MAX_NAME_LEN);

    return filter;
}

struct io_filter_t *
get_io_filter(struct io_filter_t *filter, const char *name)
{
    if (!name) {
        return NULL;
    }

    while (filter) {
        if (strcmp(filter->name, name) == 0) {
            return filter;
        }
        filter = filter->next;
    }

    return NULL;
}

void
io_filter_disable(struct io_filter_t *filter, const char *name)
{
    struct io_filter_t *f = NULL;

    if (!name) {
        f = filter;
    } else {
        f = get_io_filter(filter, name);
    }

    if (!f) {
        return;
    }

    f->enabled = 0;
}

void
io_filter_enable(struct io_filter_t *filter, const char *name)
{
    struct io_filter_t *f = NULL;

    if (!name) {
        f = filter;
    } else {
        f = get_io_filter(filter, name);
    }

    if (!f) {
        return;
    }

    f->enabled = 1;
}

void
register_read_filter(IO_HANDLE h, io_filter_fn fn, const char *name)
{
    const IOM *machine = get_machine_ref(h);
    if (!machine) {
        printf("Error: Could not find IO Machine with handle %d\n", h);
        //return;
    }

    printf("Registering read filter \"%s\" with %s[%d]\n", name, machine->name, h);
    machine->lock(h);
    struct io_desc *io = machine->get_read_desc(h);
    struct io_filter_t *f = (struct io_filter_t *)io->obj;
    
    // Create new filter
    struct io_filter_t *new = create_filter(io->alloc, name, fn);

    new->direction = IOF_READ;
    new->next = f;

    io->obj = new;
    machine->unlock(h);
}

void
register_write_filter(IO_HANDLE h, io_filter_fn fn, const char *name)
{
    const IOM *machine = get_machine_ref(h);

    printf("Registering write filter \"%s\" with %s[%d]\n", name, machine->name, h);
    machine->lock(h);
    struct io_desc *io = machine->get_write_desc(h);
    struct io_filter_t *f = (struct io_filter_t *)io->obj;
    
    // Create new filter
    struct io_filter_t *new = create_filter(io->alloc, name, fn);
    new->direction = IOF_WRITE;
    new->next = f;

    io->obj = new;
    machine->unlock(h);
}

void
add_read_filter(IO_HANDLE h, struct io_filter_t *addme)
{
    // Find end of filter chain
    struct io_filter_t *f = addme;
    while (f->next) {
        f->direction = IOF_READ;
        f = f->next;
    }
    addme->direction = IOF_READ;

    // Get write filter from IOM write descriptor
    const IOM *machine = get_machine_ref(h);
    machine->lock(h);
    struct io_desc *io = machine->get_read_desc(h);
    struct io_filter_t *machine_fil = (struct io_filter_t *)io->obj;

    // Link the new filter chain to the "write" input filter
    f->next = machine_fil;

    // Add the new filter as the new input
    io->obj = addme;
    machine->unlock(h);
}

void
add_write_filter(IO_HANDLE h, struct io_filter_t *addme)
{
    // Find end of filter chain
    struct io_filter_t *f = addme;
    while (f->next) {
        f->direction = IOF_WRITE;
        f = f->next;
    }
    addme->direction = IOF_WRITE;

    // Get write filter from IOM write descriptor
    const IOM *machine = get_machine_ref(h);
    machine->lock(h);
    struct io_desc *io = machine->get_write_desc(h);
    struct io_filter_t *machine_fil = (struct io_filter_t *)io->obj;

    // Link the new filter chain to the "write" input filter
    f->next = machine_fil;

    // Add the new filter as the new input
    io->obj = addme;
    machine->unlock(h);
}

static int
feedback_metric_fn(IO_FILTER_ARGS)
{
    // Dereference filter variables
    IO_FILTER *metric = (IO_FILTER *)IO_FILTER_ARGS_FILTER->obj;

    int ret = CALL_FILTER_PASS_ARGS(metric);

    if (ret == IO_SUCCESS) {
        ret = CALL_NEXT_FILTER();
    }

    return ret;
}

static int
feedback_controller_fn(IO_FILTER_ARGS)
{
    int ret = IO_ERROR;

    // Dereference filter variables
    enum io_filter_direction dir = IO_FILTER_ARGS_FILTER->direction;
    struct fb_ctl_t *fb = (struct fb_ctl_t *)IO_FILTER_ARGS_FILTER->obj;
    POOL *p = (POOL *)IO_FILTER_ARGS_FILTER->alloc;

    // Loops must be WRITE filters
    if (dir == IOF_READ) {
        printf("ERROR: %s can not be used as a read filter\n", __FUNCTION__);
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Save off original buffer and size
    char *caller_buf = (char *)IO_FILTER_ARGS_BUF;
    size_t caller_bytes = *IO_FILTER_ARGS_BYTES;

    // Allocate memory for internal buffer
    if (fb->bytes < caller_bytes) {
        fb->filter_buf = repalloc(fb->filter_buf, caller_bytes, p);
    }

    // Copy original buffer, and call next filter, as long as the
    // feedback_metric mechanism responds with IO_CONTINUE
    do {
        memcpy(fb->filter_buf, caller_buf, caller_bytes);
        *IO_FILTER_ARGS_BYTES = caller_bytes;
        ret = CALL_NEXT_FILTER_BUF(fb->filter_buf, IO_FILTER_ARGS_BYTES);
    } while (ret == IO_CONTINUE);

    return ret;
}

static struct io_filter_t *
add_feedback_controller(void *alloc, struct io_filter_t *head, struct io_filter_t *feedback,
    struct io_filter_t *metric)
{
    // Create controller filter
    IO_FILTER *fc = create_filter(alloc, "feedback_controller", feedback_controller_fn);
    struct fb_ctl_t *ctl = (struct fb_ctl_t *)palloc(alloc, sizeof(struct fb_ctl_t));
    ctl->filter_buf = NULL;
    ctl->bytes = 0;
    fc->obj = ctl;

    // Create metric filter
    IO_FILTER *fm = create_filter(alloc, "feedback_metric", feedback_metric_fn);
    fm->obj = metric;

    // Add feedback controller after feedback filter
    fm->next = feedback->next;
    feedback->next = fm;

    // Add feedback controller before feedback target (head)
    fc->next = head;

    return fc;
}

void
add_feedback_write_filter(IO_HANDLE h, struct io_filter_t *addme,
    struct io_filter_t *feedback, struct io_filter_t *feedback_metric)
{
    // Get write filter from IOM write descriptor
    const IOM *machine = get_machine_ref(h);
    machine->lock(h);
    struct io_desc *io = machine->get_write_desc(h);
    struct io_filter_t *machine_fil = (struct io_filter_t *)io->obj;

    // Find feedback filter in machine filter
    struct io_filter_t *f = machine_fil;
    while (f) {
        if (f == feedback) {
            break;
        }
        f = f->next;
    }

    // If the feedback filter can't be found, return with error message
    if (!f) {
        printf("ERROR: Could not find feedback filter in IOM %d\n", h);
        return;
    }

    // Find end of filter chain
    f = addme;
    while (f->next) {
        f->direction = IOF_WRITE;
        f = f->next;
    }
    addme->direction = IOF_WRITE;

    // Link the new filter chain to the "write" input filter
    f->next = machine_fil;

    // Create the feedback controller
    struct io_filter_t *fbc = add_feedback_controller(io->alloc, addme, feedback, feedback_metric);

    // Add the new filter as the new input
    io->obj = fbc;
    machine->unlock(h);
}

struct io_filter_t *
filter_read_init(POOL *p, const char *name, io_filter_fn fn, IO_DESC *d)
{
    if (!d->io_read) {
        printf("Failed to initialize read filter\n");
        return NULL;
    }

    struct io_filter_t *filter;
    filter = create_filter(p, name, fn);
    if (!filter) {
        printf("Failed to initialize read filter\n");
        return NULL;
    }
    d->io_read->obj = filter;
    return filter;
}

struct io_filter_t *
filter_write_init(POOL *p, const char *name, io_filter_fn fn, IO_DESC *d)
{
    if (!d->io_write) {
        printf("Failed to initialize write filter\n");
        return NULL;
    }

    struct io_filter_t *filter;
    filter = create_filter(p, name, fn);
    if (!filter) {
        printf("Failed to initialize write filter\n");
        return NULL;
    }
    d->io_write->obj = filter;
    return filter;
}
