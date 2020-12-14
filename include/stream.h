#ifndef __BINGEWATCH_STREAM_H__
#define __BINGEWATCH_STREAM_H__

typedef int IO_STREAM;

#define BW_NOFLAGS  0x0
#define BW_BUFFERED 0x1

IO_STREAM new_stream();
int start_stream(IO_STREAM s);
int stop_stream(IO_STREAM s);
void stop_streams();
void join_stream(IO_STREAM s);
void stream_cleanup();

int io_stream_add_segment(IO_STREAM h, int in, int out, int flag);
int io_stream_add_tee_segment(IO_STREAM h, int in, int out, int out1, int flag);
void stream_set_name(IO_STREAM h, const char *name);
void stream_set_log_level(char *level);

#endif
