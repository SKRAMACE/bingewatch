#ifndef __BINGEWATCH_STREAM_H__
#define __BINGEWATCH_STREAM_H__

#define STREAM_NAME_LEN 128
extern __thread char stream_name[STREAM_NAME_LEN];

typedef int IO_STREAM;
typedef int IO_SEGMENT;

IO_STREAM new_stream();
int start_stream(IO_STREAM s);
int stop_stream(IO_STREAM s);
void stop_streams();
void join_stream(IO_STREAM s);
void stream_cleanup();

IO_SEGMENT io_stream_add_segment(IO_STREAM h, int in, int out);
IO_SEGMENT io_stream_add_tee_segment(IO_STREAM h, int in, int out0, int out1);

#endif
