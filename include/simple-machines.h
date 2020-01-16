#ifndef __BINGEWATCH_MACHINES_H__
#define __BINGEWATCH_MACHINES_H__

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
#define NO_OUTDIR NULL

enum filetype_e
{
    FILETYPE_TEXT=0,
    FILETYPE_BINARY,
};

// ARG STRUCTS
struct fileiom_args {
    char *fname;
    char *outdir;
    char is_binary;
    char is_rotate;
};

const IOM *get_file_machine();

IO_HANDLE new_file_machine(char *fname, char *outdir, enum filetype_e type);
IO_HANDLE new_rotating_file_machine(char *fname, char *outdir, enum filetype_e type);


/***** NULL MACHINE *****/
IO_HANDLE new_null_machine();

#endif
