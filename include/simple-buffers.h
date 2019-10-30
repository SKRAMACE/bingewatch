#ifndef __BINGEWATCH_BUFFERS_H__
#define __BINGEWATCH_BUFFERS_H__

// TODO: Write generic functions which look up the buffer type and return
//  size_t bw_get_buf_size(IO_HANDLE h);

#define BF_BLOCKFILL 0x1

// Continuous Variable-Size Buffer
struct rbiom_args {
    uint64_t buf_bytes;
    uint64_t block_bytes;
    uint16_t align;
};

const IOM *get_rb_machine();
uint64_t rb_get_size(IO_HANDLE h);
uint64_t rb_get_bytes(IO_HANDLE h);
void rbiom_update_defaults(struct rbiom_args *rb);
const IOM *new_rb_machine(IO_HANDLE *h, uint64_t buffer_size, uint64_t block_size);

// Fixed-size Block Buffer
struct fbbiom_args {
    uint64_t buf_bytes;
    uint64_t block_bytes;
    uint16_t align;
    uint32_t flags;
};

const IOM *get_fbb_machine();
uint64_t fbb_get_size(IO_HANDLE h);
uint64_t fbb_get_bytes(IO_HANDLE h);
//const IOM *new_fbb_machine_fill(IO_HANDLE *h, uint64_t buffer_size, uint64_t block_size);
const IOM *new_fbb_machine(IO_HANDLE *h, uint64_t buffer_size, uint64_t block_size);

// Sync Buffer
// Continuous Variable-Size Buffer
struct sync_iom_args {
    uint64_t buf_bytes;
    uint64_t block_bytes;
    uint16_t align;
};

// Asynchronous Variable-Block Buffer
struct avbb_args {
    uint64_t block_bytes;
    uint64_t block_count;
};

const IOM *get_avbb_machine();
void avbbiom_update_defaults(struct avbb_args *rb);

// Asynchronous Fixed Packet Buffer
struct afpbiom_args {
    uint64_t header_bytes;
    uint64_t payload_bytes;
    uint64_t block_count;
};

const IOM *get_afpb_machine();
void afpbiom_update_defaults(struct afpbiom_args *afpb);

struct afpb_packet {
    char *packet;
    char *header;
    char *payload;
    uint64_t header_len;
    uint64_t payload_len;
};

// Alternate access methods
int afpb_next_full_packet(IO_HANDLE h, struct afpb_packet *packet);
void afpb_done_with_packet(void *addr);

#endif
