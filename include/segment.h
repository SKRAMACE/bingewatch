#ifndef __SEGMENT_H__
#define __SEGMENT_H__

#include "stream-state.h"

#define SEG_NAME_LEN 128
extern __thread char seg_name[SEG_NAME_LEN];

typedef void (*seg_callback)(void*);
typedef void* IO_SEGMENT;

void segment_register_callback_complete(IO_SEGMENT, seg_callback fn, void *arg);
void segment_register_callback_error(IO_SEGMENT, seg_callback fn, void *arg);

IO_SEGMENT segment_create_1_1(POOL *pool, IO_HANDLE in, IO_HANDLE out);
IO_SEGMENT segment_create_1_2(POOL *pool, IO_HANDLE in, IO_HANDLE out0, IO_HANDLE out1);

void segment_start(IO_SEGMENT seg, enum stream_state_e *state);
void segment_join(IO_SEGMENT seg);
void segment_destroy(IO_SEGMENT seg);
int segment_is_running(IO_SEGMENT seg);
size_t segment_bytes(IO_SEGMENT seg);

void segment_set_log_level(char *level);

#endif
