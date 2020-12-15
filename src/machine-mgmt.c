#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "machine.h"

#define LOGEX_TAG "BW-MACHINE"
#include "bw-log.h"

#define DECLARE_CREATE_NOIMPL \
static IO_HANDLE create_noimpl(void *arg) { \
    warn("Function \"create\" not implemented"); \
}
#define CREATE_NOIMPL create_noimpl

#define DECLARE_NOIMPL(name) \
static void name##_noimpl(IO_HANDLE h) { \
    warn("Function \"" #name "\" not implemented"); \
}
#define NOIMPL(name) name##_noimpl

#define DECLARE_IO_NOIMPL(name) \
static int name##_noimpl(IO_HANDLE h, void *buf, size_t *len) { \
    warn("Function \"" #name "\" not implemented"); \
}
#define IO_NOIMPL(name) name##_noimpl

DECLARE_CREATE_NOIMPL
DECLARE_IO_NOIMPL(read)
DECLARE_IO_NOIMPL(write)
DECLARE_NOIMPL(destroy)
DECLARE_NOIMPL(lock)
DECLARE_NOIMPL(unlock)

// Map handles to machine pointers
#define MACHINE_CHUNK 0x100
static int handle_count = 0;
static int handle_alloc = 0;
static struct handle_map_t {
    IO_HANDLE handle;
    IOM *machine;
} *handle_map = NULL;

// Keep track of registered machines
static int machine_count = 0;
static IOM **machines = NULL;

static POOL *bingewatch_pool = NULL;

IOM *
get_machine(const char *name)
{
    int i;
    for (i = 0; i < machine_count; i++) {
        IOM *iom = machines[i];
        if (0 == strcmp(name, iom->name)) {
            return iom;
        }
    }
    return NULL;
}

static IOM *
get_machine_by_handle(IO_HANDLE handle)
{
    int i;
    for (i = 0; i < handle_count; i++) {
        struct handle_map_t *hm = handle_map + i;
        if (hm->handle == handle) {
            return hm->machine;
        }
    }
    return NULL;
}


const IOM *
get_machine_ref(IO_HANDLE handle)
{
    IOM *m = get_machine_by_handle(handle);
    return (const IOM*)m;
}

IOM *
machine_register(const char *name)
{
    // Init master pool
    if (!bingewatch_pool) {
        bingewatch_pool = create_pool();
    }
    
    // Check for name
    IOM *machine = get_machine(name);
    if (machine) {
        error("io machine \"%s\" already exists.", name);
        return NULL;
    }

    // Create machine
    machine = (IOM*)pcalloc(bingewatch_pool, sizeof(IOM));

    // Create memory pool for machine
    machine->alloc = create_subpool(bingewatch_pool);
    // Set name
    machine->name = (char *)palloc(machine->alloc, strlen(name) + 1);
    strcpy(machine->name, name);
    
    // Set generic functions
    machine->create  = CREATE_NOIMPL;
    machine->read    = machine_desc_read;
    machine->write   = machine_desc_write;
    machine->stop    = machine_stop;
    machine->destroy = NOIMPL(destroy);

    machine->lock    = machine_lock;
    machine->unlock  = machine_unlock;
    machine->get_read_desc  = machine_get_read_desc;
    machine->get_write_desc = machine_get_write_desc;

    machine->buf_read_size_rec = 0;
    machine->buf_write_size_rec = 0;

    // Track new machine 
    machine_count++;
    machines = (IOM**)repalloc(machines, machine_count * sizeof(IOM*), bingewatch_pool);
    machines[machine_count - 1] = machine;

    return machine;
}

IO_HANDLE
request_handle(IOM *machine)
{
    static size_t handle_counter = 0;

    if (!handle_map) {
        handle_alloc = MACHINE_CHUNK;
        handle_map = (struct handle_map_t *)palloc(bingewatch_pool, handle_alloc * sizeof(struct handle_map_t));
    }

    if (handle_count == handle_alloc) {
        handle_alloc += MACHINE_CHUNK;
        handle_map = (struct handle_map_t *)repalloc(handle_map, handle_alloc * sizeof(struct handle_map_t), bingewatch_pool);
    }

    struct handle_map_t *m = handle_map + handle_count++;
    m->handle = ++handle_counter;
    m->machine = machine;

    return m->handle;
}

void
machine_set_write_size(IO_HANDLE h, size_t len)
{
    IOM *d = get_machine_by_handle(h);
    if (!d) {
        return;
    }

    d->buf_write_size_rec = len;
}

void
machine_set_read_size(IO_HANDLE h, size_t len)
{
    IOM *d = get_machine_by_handle(h);
    if (!d) {
        return;
    }

    d->buf_read_size_rec = len;
}

void
machine_cleanup()
{
    int i = 0;   
    for (; i < handle_count; i++) {
        struct handle_map_t *hm = handle_map + i;
        if (!hm->machine->destroy) {
            continue;
        }
        hm->machine->destroy(hm->handle);
    }
}

void
machine_mgmt_set_log_level(char *level)
{
    bw_set_log_level_str(level);
}
