#ifndef __BINGEWATCH_STREAM_H__
#define __BINGEWATCH_STREAM_H__

#define SEG_NAME_LEN 128
extern __thread char seg_name[SEG_NAME_LEN];

typedef int IO_STREAM;
typedef int IO_SEGMENT;

#define BW_NOFLAGS  0x0
#define BW_BUFFERED 0x1

IO_STREAM new_stream();
int start_stream(IO_STREAM s);
int stop_stream(IO_STREAM s);
void stop_streams();
void join_stream(IO_STREAM s);
void stream_cleanup();

IO_SEGMENT io_stream_add_segment(IO_STREAM h, int in, int out, int flag);
IO_SEGMENT io_stream_add_tee_segment(IO_STREAM h, int in, int out, int out1, int flag);

#endif
