#ifndef __BINGEWATCH_MACHINES_H__
#define __BINGEWATCH_MACHINES_H__

#ifdef BINGEWATCH_LOCAL
#include "machine.h"
#include "filter.h"
#include "file-machine.h"
#include "fifo-machine.h"
#include "socket-machine.h"
#else
#include <bingewatch/machine.h>
#include <bingewatch/filter.h>
#include <bingewatch/file-machine.h>
#include <bingewatch/fifo-machine.h>
#include <bingewatch/socket-machine.h>
#endif

/***** SOCKET MACHINE *****/
IO_HANDLE new_udp_server_machine(int ip, short port);
IO_HANDLE new_udp_client_machine(int ip, short port);

void set_payload_size(IO_HANDLE h, size_t payload_size);

/***** FILE MACHINE *****/
IO_HANDLE new_file_write_machine(char *rootdir, char *fname, char *ext);
IO_HANDLE new_file_read_machine(char *fname);

void file_iom_set_auto_rotate(IO_HANDLE h);
void file_iom_set_rotate(IO_HANDLE h);
void file_iom_set_auto_date(IO_HANDLE h);
void file_iom_set_auto_date_fmt(IO_HANDLE h, const char *fmt);
IO_FILTER *file_rotate_filter(IO_HANDLE h);
IO_FILTER *file_dir_rotate_filter(IO_HANDLE h, const char *basedir);

/***** FIFO MACHINE *****/
IO_HANDLE new_fifo_write_machine(char *fname);
IO_HANDLE new_fifo_read_machine(char *fname);

void fifo_iom_set_leave_open(IO_HANDLE h);

/***** NULL MACHINE *****/
extern const IOM *null_machine;
IO_HANDLE new_null_machine();

#endif
