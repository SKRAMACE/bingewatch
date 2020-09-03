#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <unistd.h>

#include "machine.h"
#include "filter.h"
#include "socket-machine.h"
#include "simple-buffers.h"

const IOM *socket_machine;
static IOM *_socket_machine = NULL;

static size_t default_payload_size = 0;

static size_t default_tcp_size = TCP_PACKET_SIZE;

static struct sock_desc_t {
    int fd;
    enum sock_type_e type;
    enum sock_protocol_e protocol;
    struct sockaddr_in addr;
    size_t payload_size;
    struct sock_desc_t *next;        // Pointer to next ring descriptor

    IO_HANDLE handle;         // IO Handle for this buffer
    char in_use;              // Flag designates sock memory in use
    pthread_mutex_t lock;     // Mutex lock for this socket
    struct io_desc *io_read;  // IO read descriptor for this buffer
    struct io_desc *io_write; // IO write descriptor for this buffer
    POOL *pool;               // Memory management pool
} *socks = NULL;

static pthread_mutex_t sock_list_lock = PTHREAD_MUTEX_INITIALIZER;

static inline void
acquire_socket(struct sock_desc_t *s)
{
    pthread_mutex_lock(&s->lock);
    s->in_use++;
    pthread_mutex_unlock(&s->lock);
}

static inline void
release_socket(struct sock_desc_t *s)
{
    pthread_mutex_lock(&s->lock);
    s->in_use--;
    pthread_mutex_unlock(&s->lock);
}

// Add a new socket descriptor
static void
add_socket(struct sock_desc_t *sock)
{
    pthread_mutex_lock(&sock_list_lock);
    struct sock_desc_t *s = socks;
    if (!s) {
        socks = sock;
    } else {
        while (s->next) {
            s = s->next;
        }
        s->next = sock;
    }
    pthread_mutex_unlock(&sock_list_lock);
}

// Get a pointer to a socket with the specified handle
static struct sock_desc_t *
get_socket(IO_HANDLE h)
{
    pthread_mutex_lock(&sock_list_lock);
    struct sock_desc_t *s = socks;

    while (s) {
        IO_HANDLE sh = s->handle;

        if (sh == h) {
            break;
        }

        s = s->next;
    }
    pthread_mutex_unlock(&sock_list_lock);

    return s;
}

static void
free_sock(struct sock_desc_t *sock) {
    if (!sock) {
        return;
    }

    pthread_mutex_destroy(&sock->lock);
    pfree(sock->pool);
}

// Free the sock descriptor 
static void
destroy_sock(IO_HANDLE h)
{
    pthread_mutex_lock(&sock_list_lock);
    struct sock_desc_t *s = socks;

    struct sock_desc_t *sp = NULL;
    while (s) {
        if (s->handle == h) {
            break;
        }
        sp = s;

        s = s->next;
    }

    // handle not found
    if (!s) {
        return;

    // handle in first slot
    } else if (!sp) {
        socks = s->next;

    } else if (s && sp) {
        sp->next = s->next;
    }
    pthread_mutex_unlock(&sock_list_lock);

    while (s->in_use) {
        continue;
    }

    shutdown(s->fd, SHUT_RDWR);
    close(s->fd);

    pthread_mutex_lock(&sock_list_lock);
    free_sock(s);
    pthread_mutex_unlock(&sock_list_lock);
}

static int
udp_read(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get socket from handle
    struct sock_desc_t *sock = get_socket(*handle);
    if (!sock) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    size_t remaining = *IO_FILTER_ARGS_BYTES;
    size_t total_read = 0;

    while (remaining) {
        size_t b = (remaining > sock->payload_size) ? sock->payload_size : remaining;

        struct sockaddr_in remote;
        socklen_t len = sizeof(remote);

        pthread_mutex_lock(&sock->lock);
        struct sockaddr *addr = (struct sockaddr *)&remote;
        size_t bytes_rcvd = recvfrom(sock->fd, IO_FILTER_ARGS_BUF, b, 0, addr, &len);
        pthread_mutex_unlock(&sock->lock);

        if (bytes_rcvd == 0) {
            *IO_FILTER_ARGS_BYTES = total_read;
            return IO_SUCCESS;
        } else if (bytes_rcvd < 0) {
            perror("recvfrom failed");
            *IO_FILTER_ARGS_BYTES = 0;
            return IO_ERROR;
        }

        remaining -= bytes_rcvd;
        IO_FILTER_ARGS_BUF += bytes_rcvd;
        total_read += bytes_rcvd;
    }

    return IO_SUCCESS;
}

static int
udp_write(IO_FILTER_ARGS)
{
    // Get filter data from filter
    IO_HANDLE *handle = (IO_HANDLE *)IO_FILTER_ARGS_FILTER->obj;

    // Get socket from handle
    struct sock_desc_t *sock = get_socket(*handle);
    if (!sock) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    size_t written = 0;
    size_t remaining = *IO_FILTER_ARGS_BYTES;

    pthread_mutex_lock(&sock->lock);
    while (remaining) {
        size_t b = (remaining > sock->payload_size) ? sock->payload_size : remaining;

        struct sockaddr *addr = (struct sockaddr *)&sock->addr;
        int bytes_sent = sendto(sock->fd, IO_FILTER_ARGS_BUF, b, 0, addr, sizeof(struct sockaddr_in));

        if (bytes_sent < 0) {
            perror("sendto failed");
            *IO_FILTER_ARGS_BYTES = 0;
            return IO_ERROR;
        }
        remaining -= bytes_sent;
        IO_FILTER_ARGS_BUF += bytes_sent;
        written += bytes_sent;
    }
    pthread_mutex_unlock(&sock->lock);

    *IO_FILTER_ARGS_BYTES = written;
    return IO_SUCCESS;
}

static int
tcp_read(IO_FILTER_ARGS)
{
    return IO_SUCCESS;
}

static int
tcp_write(IO_FILTER_ARGS)
{
    return IO_SUCCESS;
}

/*
 * Create new filters to interface directly with the socket
 */
static enum io_status
init_filters(struct sock_desc_t *sock)
{
    POOL *fpool = create_subpool(sock->pool);

    struct io_desc *d = NULL;
    d = (struct io_desc *)pcalloc(fpool, sizeof(struct io_desc));
    if (!d) {
        printf("ERROR: Failed to initialize socket filter\n");
        goto failure;
    }
    d->alloc = fpool;

    const char *filter_str = (sock->protocol == AF_INET_UDP) ? "_udp" :
                             (sock->protocol == AF_INET_TCP) ? "_tcp" : NULL;
    
    io_filter_fn filter_fn = NULL;
    switch (sock->type) {
    case SOCK_TYPE_SERVER:
        sock->io_read = d;
        filter_fn = (sock->protocol == AF_INET_UDP) ? udp_read :
                    (sock->protocol == AF_INET_TCP) ? tcp_read : NULL;
        break;
    case SOCK_TYPE_CLIENT:
        sock->io_write = d;
        filter_fn = (sock->protocol == AF_INET_UDP) ? udp_write :
                    (sock->protocol == AF_INET_TCP) ? tcp_write : NULL;
        break;
    default:
        printf("ERROR: Unknown socket type (%d)\n", sock->type);
        goto failure;
    }

    if (!filter_fn) {
        printf("ERROR: Unknown socket protocol (%d)\n", sock->protocol);
        goto failure;
    }

    struct io_filter_t *fil = create_filter(fpool, filter_str, filter_fn);
    if (!fil) {
        printf("ERROR: Failed to initialize socket filter\n");
        goto failure;
    }

    // Set socket handle as the filter object
    IO_HANDLE *h;
    h = palloc(fil->alloc, sizeof(IO_HANDLE));
    *h = sock->handle;
    fil->obj = h;

    if (sock->type == SOCK_TYPE_SERVER) {
        sock->io_read->obj = fil;
    } else {
        sock->io_write->obj = fil;
    }

    return IO_SUCCESS;

failure:
    pfree(fpool);
    return IO_ERROR;
}

static IO_HANDLE
create_sock(void *arg)
{
    struct sockiom_args *args = (struct sockiom_args *)arg;

    POOL *p = create_subpool(_socket_machine->alloc);
    if (!p) {
        printf("ERROR: Failed to create memory pool\n");
        return 0;
    }

    struct sock_desc_t *sock = pcalloc(p, sizeof(struct sock_desc_t));
    if (!sock) {
        printf("ERROR: Failed to allocate %#zx bytes for sock descriptor\n", sizeof(struct sock_desc_t));
        pfree(p);
        return 0;
    }

    pthread_mutex_init(&sock->lock, NULL);
    sock->pool = p;

    // Create an IPv4 Socket
    sock->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock->fd < 0) {
        printf("ERROR: socket error\n");
        return IO_ERROR;
    }

    // Set socket parameters
    sock->addr.sin_family = AF_INET;
    sock->addr.sin_addr.s_addr = htonl(args->ip);
    sock->addr.sin_port = htons(args->port);
    sock->protocol = args->protocol;
    sock->type = args->type;
    sock->payload_size = args->payload_size;

    // Attempt to bind to socket (server only)
    if (sock->type == SOCK_TYPE_SERVER) {
        struct sockaddr *addr = (struct sockaddr *)&sock->addr;
        if (bind(sock->fd, addr, sizeof(struct sockaddr_in)) != 0) {
            printf("ERROR: Failed to bind to " IP_FMT_STR "\n",
                IP_FMT(sock->addr.sin_addr.s_addr, sock->addr.sin_port));
            return 0;
        }
    }

    sock->handle = request_handle(_socket_machine);

    if (init_filters(sock) != IO_SUCCESS) {
        printf("ERROR: Failed to initialize filters\n");
        pfree(p);
        return 0;
    }

    add_socket(sock);
    return sock->handle;
}

static int
sock_read(IO_HANDLE h, void *buf, size_t *len)
{
    struct sock_desc_t *sock = get_socket(h);
    if (!sock) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_socket(sock);
    struct io_filter_t *f = (struct io_filter_t *)sock->io_read->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_socket(sock);

    return status;
}

/*
 * Write "len" bytes to the socket with handle==h
 */
static int
sock_write(IO_HANDLE h, void *buf, size_t *len)
{
    struct sock_desc_t *sock = get_socket(h);
    if (!sock) {
        *len = 0;
        return IO_ERROR;
    }

    acquire_socket(sock);
    struct io_filter_t *f = (struct io_filter_t *)sock->io_write->obj;
    int status = f->call(f, buf, len, IO_NO_BLOCK, IO_DEFAULT_ALIGN);
    release_socket(sock);

    return status;
}

/*
 * Fills the struct with a copy of the current machine
 */
const IOM*
get_sock_machine()
{
    IOM *machine = _socket_machine;
    if (!machine) {
        machine = machine_register("socket");

        machine->create = create_sock;
        machine->destroy = destroy_sock;
        machine->stop = machine_disable_read;
        machine->read = sock_read;
        machine->write = sock_write;
        machine->obj = NULL;

        _socket_machine = machine;
        socket_machine = machine;
    }
    return (const IOM *)machine;
}

void
set_payload_size(IO_HANDLE h, size_t payload_size)
{
    struct sock_desc_t *sd = (struct sock_desc_t *)machine_get_desc(h);
    if (!sd) {
        return;
    }

    pthread_mutex_t *lock = &sd->lock;
    pthread_mutex_lock(lock);

    sd->payload_size = payload_size;

    pthread_mutex_unlock(lock);
}

IO_HANDLE
new_socket_machine(int ip, short port, size_t payload_size,
    enum sock_type_e type, enum sock_protocol_e protocol)
{
    struct sockiom_args args = {
        .ip = ip,
        .port = port,
        .type = type,
        .protocol = protocol,
        .payload_size = payload_size,
    };

    const IOM *sm = get_sock_machine();
    return sm->create(&args);
}

IO_HANDLE
new_udp_server_machine(int ip, short port)
{
    return new_socket_machine(ip, port, UDP_PACKET_SIZE, SOCK_TYPE_SERVER, AF_INET_UDP);
}

IO_HANDLE
new_udp_client_machine(int ip, short port)
{
    return new_socket_machine(ip, port, UDP_PACKET_SIZE, SOCK_TYPE_CLIENT, AF_INET_UDP);
}
