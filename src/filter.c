#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "radpool.h"
#include "machine.h"
#include "filter.h"

/*
// TODO: Explore the option of using macros, helpers, or templates to streamline filter creation
#define DECLARE_FILTER(name) \
    int _filter_##name(IO_FILTER_ARGS);\
    int filter_##name(IO_FILTER_ARGS) {\
        if (!f->enabled) { return CALL_NEXT_FILTER(); }\
    }\
    int _filter_##name(IO_FILTER_ARGS)

DECLARE_FILTER(generic)
{
    CALL_NEXT_FILTER();
    printf("Generic Stuff\n");
}
*/

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
