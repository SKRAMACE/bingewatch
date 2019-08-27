#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <unistd.h>

#include "radpool.h"
#include "machine.h"
#include "filter.h"
#include "simple-machines.h"
#include "simple-buffers.h"

static IOM *sock_machine = NULL;

static enum af_inet_type default_socktype = AF_INET_NONE;
static uint32_t default_payload_size = 0;

static uint64_t default_udp_size = UDP_PACKET_SIZE;
static uint64_t default_tcp_size = TCP_PACKET_SIZE;

static struct sock_t {
    int fd; 
    enum af_inet_type socktype;
    struct sockaddr_in srv_addr;
    struct sockaddr_in rem_addr;
    uint32_t payload_size;
    struct sock_t *next;        // Pointer to next ring descriptor

    IO_HANDLE handle;         // IO Handle for this buffer
    char in_use;              // Flag designates sock memory in use
    pthread_mutex_t lock;     // Mutex lock for this socket
    struct io_desc *io_read;  // IO read descriptor for this buffer
    struct io_desc *io_write; // IO write descriptor for this buffer
    POOL *pool;               // Memory management pool
} *socks = NULL;

static pthread_mutex_t sock_list_lock = PTHREAD_MUTEX_INITIALIZER;

static inline void
sanitize_args(struct sockiom_args *args)
{
    switch(args->socktype) {
    case AF_INET_UDP:
        if (0 == args->payload_size) {
            args->payload_size = default_udp_size;
        }
    case AF_INET_TCP:
        if (0 == args->payload_size) {
            args->payload_size = default_tcp_size;
        }
        break;
    default:
        args->socktype = default_socktype;
        args->payload_size = default_payload_size;
    }
}

static inline void
acquire_socket(struct sock_t *s)
{
    pthread_mutex_lock(&s->lock);
    s->in_use++;
    pthread_mutex_unlock(&s->lock);
}

static inline void
release_socket(struct sock_t *s)
{
    pthread_mutex_lock(&s->lock);
    s->in_use--;
    pthread_mutex_unlock(&s->lock);
}

// Add a new socket descriptor
static void
add_socket(struct sock_t *sock)
{
    pthread_mutex_lock(&sock_list_lock);
    struct sock_t *s = socks;
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
static struct sock_t *
get_socket(IO_HANDLE h)
{
    pthread_mutex_lock(&sock_list_lock);
    struct sock_t *s = socks;

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
free_sock(struct sock_t *sock) {
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
    struct sock_t *s = socks;

    struct sock_t *sp = NULL;
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
    struct sock_t *sock = get_socket(*handle);
    if (!sock) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    uint64_t remaining = *IO_FILTER_ARGS_BYTES;
    uint64_t total_read = 0;

    while (remaining) {
        uint64_t b = (remaining > sock->payload_size) ? sock->payload_size : remaining;

        struct sockaddr_in remote;
        socklen_t len = sizeof(remote);

        pthread_mutex_lock(&sock->lock);
        struct sockaddr *addr = (struct sockaddr *)&remote;
        uint32_t bytes_rcvd = recvfrom(sock->fd, IO_FILTER_ARGS_BUF, b, 0, addr, &len);
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
    struct sock_t *sock = get_socket(*handle);
    if (!sock) {
        *IO_FILTER_ARGS_BYTES = 0;
        return IO_ERROR;
    }

    uint64_t written = 0;
    uint64_t remaining = *IO_FILTER_ARGS_BYTES;

    pthread_mutex_lock(&sock->lock);
    while (remaining) {
        uint64_t b = (remaining > sock->payload_size) ? sock->payload_size : remaining;

        struct sockaddr *addr = (struct sockaddr *)&sock->rem_addr;
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
init_filters(struct sock_t *sock)
{
    // Create io descriptors
    if (sock->srv_addr.sin_addr.s_addr && sock->srv_addr.sin_port) {
        sock->io_read = (struct io_desc *)pcalloc(sock->pool, sizeof(struct io_desc));
        if (!sock->io_read) {
            printf("Failed to initialize read descriptor\n");
            return IO_ERROR;
        }
        sock->io_read->alloc = sock->pool;
    }

    if (sock->rem_addr.sin_addr.s_addr && sock->rem_addr.sin_port) {
        sock->io_write = (struct io_desc *)pcalloc(sock->pool, sizeof(struct io_desc));
        if (!sock->io_write) {
            printf("Failed to initialize read descriptor\n");
            return IO_ERROR;
        }
        sock->io_write->alloc = sock->pool;
    }
    
    // Create base filters
    struct io_filter_t *fil;
    if (sock->io_read) {
        switch (sock->socktype) {
        case AF_INET_UDP:
            fil = create_filter(sock->pool, "_udp", udp_read);
            break;
        case AF_INET_TCP:
            fil = create_filter(sock->pool, "_tcp", tcp_read);
            break;
        default:
            fil = NULL;
            printf("Invalid socket type\n");
        }

        if (!fil) {
            printf("Failed to initialize read filter\n");
            return IO_ERROR;
        }

        // Set socket handle as the filter object
        IO_HANDLE *h;
        h = palloc(fil->alloc, sizeof(IO_HANDLE));
        *h = sock->handle;
        fil->obj = h;

        // Set filters as io_desc object
        sock->io_read->obj = fil;
    }

    if (sock->io_write) {
        switch (sock->socktype) {
        case AF_INET_UDP:
            fil = create_filter(sock->pool, "_udp", udp_write);
            break;
        case AF_INET_TCP:
            fil = create_filter(sock->pool, "_tcp", tcp_write);
            break;
        default:
            fil = NULL;
            fil = NULL;
            printf("Invalid socket type\n");
        }

        if (!fil) {
            printf("Failed to initialize write filter\n");
            return IO_ERROR;
        }

        // Set socket handle as the filter object
        IO_HANDLE *h;
        h = palloc(fil->alloc, sizeof(IO_HANDLE));
        *h = sock->handle;
        fil->obj = h;

        // Set filters as io_desc object
        sock->io_write->obj = fil;
    }

    return IO_SUCCESS;
}

static enum io_status
create_udp(struct sock_t *udp, struct sockiom_args *args)
{
    printf("Creating new UDP socket: " IP_FMT_STR "\n", IP_FMT(args->rem_ip, args->rem_port));

    udp->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp->fd < 0) {
        perror("socket error");
        return IO_ERROR;
    }

    udp->srv_addr.sin_family = AF_INET;
    udp->srv_addr.sin_addr.s_addr = htonl(args->srv_ip);
    udp->srv_addr.sin_port = htons(args->srv_port);

    udp->rem_addr.sin_family = AF_INET;
    udp->rem_addr.sin_addr.s_addr = htonl(args->rem_ip);
    udp->rem_addr.sin_port = htons(args->rem_port);

    // TODO: If there's a bind error, mark the read function as "invalid"
    if (udp->srv_addr.sin_addr.s_addr && udp->srv_addr.sin_port) {
        printf("Binding UDP socket to: " IP_FMT_STR "\n", IP_FMT(args->srv_ip, args->srv_port));
        struct sockaddr *addr = (struct sockaddr *)&udp->srv_addr;
        if (bind(udp->fd, addr, sizeof(struct sockaddr_in)) < 0) {
            perror("error binding");
            return IO_ERROR;
        }
    }

    udp->socktype = AF_INET_UDP;
    udp->payload_size = default_udp_size;

    return IO_SUCCESS;
}

static IO_HANDLE
create_sock(void *arg)
{
    struct sockiom_args *args = (struct sockiom_args *)arg;
    sanitize_args(args);

    POOL *p = create_subpool(sock_machine->alloc);
    if (!p) {
        printf("ERROR: Failed to create memory pool\n");
        return 0;
    }

    struct sock_t *sock = pcalloc(p, sizeof(struct sock_t));
    if (!sock) {
        printf("ERROR: Failed to allocate %" PRIx64 " bytes for sock descriptor\n", sizeof(struct sock_t));
        pfree(p);
        return 0;
    }

    pthread_mutex_init(&sock->lock, NULL);
    sock->pool = p;

    int status;
    switch (args->socktype) {
    case AF_INET_UDP:
        status = create_udp(sock, args);
        break;
    case AF_INET_TCP:
        printf("TCP not implemented\n");
        status = IO_ERROR;
        break;
    default:
        status = IO_ERROR;
    }

    if (status != IO_SUCCESS) {
        pfree(p);
        return 0;
    }

    sock->handle = request_handle(sock_machine);

    status = init_filters(sock);
    if (status != IO_SUCCESS) {
        printf("ERROR: Failed to initialize filters\n");
        pfree(p);
        return 0;
    }

    add_socket(sock);
    return sock->handle;
}

static int
sock_read(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct sock_t *sock = get_socket(h);
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
sock_write(IO_HANDLE h, void *buf, uint64_t *len)
{
    struct sock_t *sock = get_socket(h);
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

void
sockiom_update_defaults(struct sockiom_args *s)
{
    sanitize_args(s);

    pthread_mutex_lock(&sock_list_lock);
    default_socktype = s->socktype;
    default_payload_size = s->payload_size;
    pthread_mutex_unlock(&sock_list_lock);
}

/*
 * Fills the struct with a copy of the current machine
 */
const IOM*
get_sock_machine()
{
    IOM *machine = sock_machine;
    if (!machine) {
        machine = machine_register("socket");

        machine->create = create_sock;
        machine->destroy = destroy_sock;
        machine->stop = machine_disable_read;
        machine->read = sock_read;
        machine->write = sock_write;
        machine->obj = NULL;

        sock_machine = machine;
    }
    return (const IOM *)machine;
}
