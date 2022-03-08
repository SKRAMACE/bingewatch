#ifndef __RING_BUF_H__
#define __RING_BUF_H__

#include "simple-buffers.h"
#include "block-list-buffer.h"

int rb_acquire_write_block(IO_HANDLE h, size_t init_bytes, const struct __block_t **b);
void rb_release_write_block(IO_HANDLE h, size_t bytes);

#endif
