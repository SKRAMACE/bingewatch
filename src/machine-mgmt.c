#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "radpool.h"
#include "machine.h"

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
get_iom(const char *name)
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

const IOM *
get_iom_ref(IO_HANDLE handle)
{
    int i;
    for (i = 0; i < handle_count; i++) {
        struct handle_map_t *hm = handle_map + i;
        if (hm->handle == handle) {
            return (const IOM*)hm->machine;
        }
    }
    return NULL;
}

IOM *
iom_register(const char *name)
{
    // Init master pool
    if (!bingewatch_pool) {
        bingewatch_pool = create_pool();
    }
    
    // Check for name
    IOM *iom = get_iom(name);
    if (iom) {
        printf("ERROR: io machine \"%s\" already exists.", name);
        return NULL;
    }

    // New machine
    iom = (IOM*)palloc(bingewatch_pool, sizeof(IOM));
    iom->alloc = create_subpool(bingewatch_pool);
    iom->name = (char *)palloc(iom->alloc, strlen(name) + 1);
    strcpy(iom->name, name);
    iom->obj = NULL;

    // Track new machine 
    machine_count++;
    machines = (IOM**)repalloc(machines, machine_count * sizeof(IOM*), bingewatch_pool);
    machines[machine_count - 1] = iom;

    return iom;
}

IO_HANDLE
request_handle(IOM *machine)
{
    static uint64_t handle_counter = 0;

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
iom_cleanup()
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
