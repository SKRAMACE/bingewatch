#ifndef __SEGMENT_H__
#define __SEGMENT_H__

#include "stream-state.h"

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

void segment_set_default_buflen(IO_SEGMENT seg, size_t len);
void segment_set_group(IO_SEGMENT seg, char **group);
void segment_set_name(IO_SEGMENT seg, char *name);
void segment_enable_metrics(IO_SEGMENT seg);
void segment_print_metrics(IO_SEGMENT seg);

#endif
