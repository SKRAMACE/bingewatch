#include <stdio.h>

#include "machine.h"
#include "simple-machines.h"
#include "filter.h"

const IOM *null_machine;
static IOM *_null_machine = NULL;

static int
null_read(IO_FILTER_ARGS)
{
    printf("ERROR: Cannot read from a \"null machine\"\n");
    return IO_ERROR;
}

static int
null_write(IO_FILTER_ARGS)
{
    return IO_SUCCESS;
}

static IO_HANDLE
create_null(void *arg)
{
    POOL *p = create_subpool(_null_machine->alloc);
    if (!p) {
        printf("ERROR: Failed to create memory pool\n");
        return 0;
    }

    struct machine_desc_t *desc = pcalloc(p, sizeof(struct machine_desc_t));
    if (!desc) {
        printf("ERROR: Failed to allocate %#zx bytes for file descriptor\n", sizeof(struct machine_desc_t));
        free_pool(p);
        return 0;
    }

    pthread_mutex_init(&desc->lock, NULL);
    desc->pool = p;

    if (machine_desc_init(p, _null_machine, desc) < IO_SUCCESS) {
        free_pool(p);
        return 0;
    }

    if (!filter_read_init(p, "_null", null_read, desc)) {
        printf("ERROR: Failed to initialize read filter\n");
        free_pool(p);
        return 0;
    }

    if (!filter_write_init(p, "_null", null_write, desc)) {
        printf("ERROR: Failed to initialize write filter\n");
        free_pool(p);
        return 0;
    }

    IO_HANDLE h;

    machine_register_desc(desc, &h);

    return h;
}

static void
destroy_null(IO_HANDLE h)
{
    return;
}

static size_t
get_bytes(IO_HANDLE h)
{
    return 0;
}

const IOM*
get_null_machine()
{
    IOM *machine = _null_machine;
    if (!machine) {
        machine = machine_register("null");

        machine->create = create_null;
        machine->destroy = destroy_null;
        machine->stop = machine_disable_read;
        machine->read = machine_desc_read;
        machine->write = machine_desc_write;
        machine->obj = NULL;

        _null_machine = machine;
        null_machine = machine;
    }
    return (const IOM *)machine;
}


IO_HANDLE
new_null_machine()
{
    const IOM *m = get_null_machine();
    return m->create(NULL);
}
