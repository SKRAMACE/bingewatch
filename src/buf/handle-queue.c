#include "machine.h"
#include "filter.h"
#include "simple-buffers.h"

#define LOGEX_TAG "HQUEUE"
#include "logging.h"
#include "bw-log.h"

const IOM *hq_machine;
static IOM *_handle_queue_machine = NULL;

struct hq_t {
    IO_DESC _b;  // IOM Descriptor
    MLIST *list;

    int flush;              // Flag used to keep reading available until the buffer is empty
};

static void
destroy_hq_machine(IO_HANDLE h)
{
    machine_destroy_desc(h);
}

static int
queue_write(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get ring from handle
    struct machine_desc_t *d = machine_get_desc(*handle);
    struct hq_t *q = (struct hq_t *)d;
    if (!q) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    char *data = IO_FILTER_ARGS_BUF; 
    size_t bytes = *IO_FILTER_ARGS_BYTES;

    HQ_ENTRY e;
    gettimeofday(&e.tv, NULL);
    e.pool = create_subpool(d->pool);
    e.buf = palloc(e.pool, bytes);
    e.bytes = bytes;
    memcpy(e.buf, data, bytes);

    if (memex_list_push(q->list, &e) != 0) {
        free_pool(e.pool);
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    return IO_SUCCESS;
}

static int
queue_read(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get ring from handle
    struct machine_desc_t *d = machine_get_desc(*handle);
    struct hq_t *q = (struct hq_t *)d;
    if (!q) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    char *data = IO_FILTER_ARGS_BUF; 
    size_t bytes = *IO_FILTER_ARGS_BYTES;
    if (bytes < sizeof(HQ_ENTRY)) {
        error("Insufficient bytes in return buffer (expected %d, got %zd)", sizeof(HQ_ENTRY), bytes);
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    HQ_ENTRY e;
    e.buf = NULL;
    uint32_t N;
    if (memex_list_pop(q->list, &e, &N) != 0) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    // Always return data
    if (N > 0) {
        memcpy(data, &e, sizeof(HQ_ENTRY));
        *IO_FILTER_ARGS_BYTES = sizeof(HQ_ENTRY);
        return IO_SUCCESS;

    // If no data, and buffer is set to "flush" disable the buffer
    } else if (d->flush) {
        io_desc_set_state(d, d->io_read, IO_DESC_DISABLING);
        return IO_COMPLETE;

    // If there is no data during normal operation, successfully return 0 bytes
    } else {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_SUCCESS;
    }
}

static IO_HANDLE
create_queue(void *arg)
{
    IO_HANDLE h = 0;

    // Create a new pool for this buffer
    POOL *p = create_subpool(_handle_queue_machine->alloc);
    if (!p) {
        error("Failed to create memory pool");
        return 0;
    }

    // Create a new buffer descriptor
    struct hq_t *q = pcalloc(p, sizeof(struct hq_t));
    if (!q) {
        error("Failed to allocate memory");
        goto free_and_return;
    }

    // Create internal queue
    int type = HQ_TYPE_FIFO;
    struct hqiom_args *a = (struct hqiom_args *)arg;
    if (a && a->type == HQ_TYPE_FIFO) {
        q->list = memex_fifo_create(p, sizeof(HQ_ENTRY));
    } else if (a && a->type == HQ_TYPE_STACK) {
        q->list = memex_stack_create(p, sizeof(HQ_ENTRY));
    } else {
        q->list = memex_fifo_create(p, sizeof(HQ_ENTRY));
    }

    if (machine_desc_init(p, _handle_queue_machine, (IO_DESC *)q) < IO_SUCCESS) {
        error("Failed to initialize mechine descriptor");
        goto free_and_return;
    }

    if (!filter_read_init(p, "ring_buf_r", queue_read, (IO_DESC *)q)) {
        error("Failed to initialize read filter");
        goto free_and_return;
    }

    if (!filter_write_init(p, "ring_buf_w", queue_write, (IO_DESC *)q)) {
        error("Failed to initialize write filter");
        goto free_and_return;
    }

    machine_register_desc((IO_DESC *)q, &h);
    return h;

free_and_return:
    free_pool(p);
    return 0;
}

static void
stop_queue(IO_HANDLE h)
{
    struct machine_desc_t *d = machine_get_desc(h);
    if (!d) {
        error("Machine %d not found", h);
        return;
    }

    // Disable writing
    if (d->io_write) {
        io_desc_set_state(d, d->io_write, IO_DESC_DISABLING);
    }

    // Allow reading until the buffer is empty
    if (d->io_read) {
        struct hq_t *q = (struct hq_t *)d;
        q->flush = 1;
    }
}

const IOM *
get_hq_machine()
{
    IOM *machine = _handle_queue_machine;
    if (!machine) {
        machine = machine_register("handle_queue");

        // Local Functions
        machine->create = create_queue;
        machine->stop = stop_queue;
        machine->destroy = destroy_hq_machine;

        _handle_queue_machine = machine;
        hq_machine = machine;
    }
    return (const IOM *)machine;
}

IO_HANDLE
new_hq_machine()
{
    const IOM *hq_machine = get_hq_machine();

    // Default hq machine is FIFO
    return hq_machine->create(NULL);
}

IO_HANDLE
new_hq_fifo_machine()
{
    const IOM *hq_machine = get_hq_machine();
    struct hqiom_args args = {HQ_TYPE_FIFO};
    return hq_machine->create(&args);
}

IO_HANDLE
new_hq_stack_machine()
{
    const IOM *hq_machine = get_hq_machine();
    struct hqiom_args args = {HQ_TYPE_STACK};
    return hq_machine->create(&args);
}
