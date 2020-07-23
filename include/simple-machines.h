#ifndef __BINGEWATCH_MACHINES_H__
#define __BINGEWATCH_MACHINES_H__

#ifdef BINGEWATCH_LOCAL
#include "machine.h"
#include "filter.h"
#include "file-machine.h"
#else
#include <bingewatch/machine.h>
#include <bingewatch/filter.h>
#include <bingewatch/file-machine.h>
#endif

#include <radpool.h>

/***** SOCKET MACHINE *****/
// IP size defines
#define MAX_MTU_SIZE 1500
#define IP_HEADER_SIZE 20
#define TCP_HEADER_SIZE 20
#define UDP_HEADER_SIZE 8
#define UDP_PACKET_SIZE (MAX_MTU_SIZE - IP_HEADER_SIZE - UDP_HEADER_SIZE)
#define TCP_PACKET_SIZE (MAX_MTU_SIZE - IP_HEADER_SIZE - TCP_HEADER_SIZE)

// IP addr helper macros
#define IP(a,b,c,d) (a<<24 | b<<16 | c<<8 | d)
#define UDP_ADDR (192<<24 | 168<<16 | 16<<8 | 99)
#define IP_FMT_STR "%d.%d.%d.%d:%d"
#define IP_FMT(ip,port) (ip>>24)&0xff, (ip>>16)&0xff, (ip>>8)&0xff, ip&0xff, port

enum af_inet_type {
    AF_INET_NONE,
    AF_INET_UDP,
    AF_INET_TCP,
};
    
struct sockiom_args {
    int srv_ip;
    short srv_port;
    int rem_ip;
    short rem_port;
    enum af_inet_type socktype;
    size_t payload_size;
};

const IOM *get_sock_machine();
void sockiom_update_defaults(struct sockiom_args *s);

/***** FILE MACHINE *****/
IO_HANDLE new_file_write_machine(char *rootdir, char *fname, char *ext);
IO_HANDLE new_file_read_machine(char *fname);

void file_iom_set_auto_rotate(IO_HANDLE h);
void file_iom_set_rotate(IO_HANDLE h);
void file_iom_set_auto_date(IO_HANDLE h);
void file_iom_set_auto_date_fmt(IO_HANDLE h, const char *fmt);
IO_FILTER *file_rotate_filter(IO_HANDLE h);
IO_FILTER *file_dir_rotate_filter(IO_HANDLE h, const char *basedir);

/***** NULL MACHINE *****/
IO_HANDLE new_null_machine();

#endif
