#ifndef __BINGEWATCH_BUFFERS_H__
#define __BINGEWATCH_BUFFERS_H__

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
