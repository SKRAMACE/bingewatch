#ifndef __BINGEWATCH_BUFFERS_H__
#define __BINGEWATCH_BUFFERS_H__

#ifdef BINGEWATCH_LOCAL
#include "machine.h"
#else
#include <bingewatch/machine.h>
#endif

#include <sys/time.h>

// TODO: Write generic functions which look up the buffer type and return
//  size_t bw_get_buf_size(IO_HANDLE h);

#define BF_BLOCKFILL 0x1

extern const IOM *rb_machine;
const IOM *get_rb_machine();
IO_HANDLE new_rb_machine();
void rb_set_high_water_mark(IO_HANDLE h, size_t bytes);

// Deprecated
size_t rb_get_size(IO_HANDLE h);
size_t rb_get_bytes(IO_HANDLE h);

// Fixed-size Block Buffer
extern const IOM *fbb_machine;
struct fbbiom_args {
    size_t buf_bytes;
    size_t block_bytes;
    uint16_t align;
    uint32_t flags;
};

const IOM *get_fbb_machine();
IO_HANDLE new_fbb_machine(size_t buffer_size, size_t block_size);

// Deprecated
size_t fbb_get_size(IO_HANDLE h);
size_t fbb_get_bytes(IO_HANDLE h);

// Sync Buffer
// Continuous Variable-Size Buffer
struct sync_iom_args {
    size_t buf_bytes;
    size_t block_bytes;
    uint16_t align;
};

// Asynchronous Variable-Block Buffer
struct avbb_args {
    size_t block_bytes;
    size_t block_count;
};

const IOM *get_avbb_machine();
void avbbiom_update_defaults(struct avbb_args *rb);

// Asynchronous Fixed Packet Buffer
struct afpbiom_args {
    size_t header_bytes;
    size_t payload_bytes;
    size_t block_count;
};

const IOM *get_afpb_machine();
void afpbiom_update_defaults(struct afpbiom_args *afpb);

struct afpb_packet {
    char *packet;
    char *header;
    char *payload;
    size_t header_len;
    size_t payload_len;
};

// Alternate access methods
int afpb_next_full_packet(IO_HANDLE h, struct afpb_packet *packet);
void afpb_done_with_packet(void *addr);

// Handle Queue
extern const IOM *hq_machine;

enum handle_queue_type_e {
    HQ_TYPE_FIFO=0,
    HQ_TYPE_STACK,
};

#define HQ_TAGLEN 64
typedef struct hqiom_entry_t {
    POOL *pool;
    void *buf;
    size_t bytes;
    struct timeval tv;
} HQ_ENTRY;

struct hqiom_args {
    int type;
};

IO_HANDLE new_hq_machine();
IO_HANDLE new_hq_fifo_machine();
IO_HANDLE new_hq_stack_machine();

int is_in_use(IO_HANDLE h);

#endif
