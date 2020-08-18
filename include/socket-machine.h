#ifndef __SOCKET_MACHINE_H__
#define __SOCKET_MACHINE_H__

// IP size defines
#define MAX_MTU_SIZE 1500
#define IP_HEADER_SIZE 20
#define TCP_HEADER_SIZE 20
#define UDP_HEADER_SIZE 8
#define UDP_PACKET_SIZE (MAX_MTU_SIZE - IP_HEADER_SIZE - UDP_HEADER_SIZE)
#define TCP_PACKET_SIZE (MAX_MTU_SIZE - IP_HEADER_SIZE - TCP_HEADER_SIZE)

// IP addr helper macros
#define IP(a,b,c,d) (a<<24 | b<<16 | c<<8 | d)
#define IP_FMT_STR "%d.%d.%d.%d:%d"
#define IP_FMT(ip,port) ip&0xff, (ip>>8)&0xff, (ip>>16)&0xff, (ip>>24)&0xff, port

enum sock_type_e {
    SOCK_TYPE_SERVER,
    SOCK_TYPE_CLIENT,
};

enum sock_protocol_e {
    AF_INET_UDP,
    AF_INET_TCP,
};

struct sockiom_args {
    int ip;
    short port;
    enum sock_type_e type;
    enum sock_protocol_e protocol;
    size_t payload_size;
};

extern const IOM *socket_machine;

const IOM *get_sock_machine();

IO_HANDLE new_socket_machine(int ip, short port, size_t payload_size,
    enum sock_type_e type, enum sock_protocol_e protocol);

#endif
