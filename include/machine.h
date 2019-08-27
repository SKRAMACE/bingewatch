#ifndef __BINGEWATCH_MACHINE_H__
#define __BINGEWATCH_MACHINE_H__

#include <pthread.h>
#include <radpool.h>

// Data Size Constants
#ifndef KB
    #define KB 1024
#endif
#ifndef MB
    #define MB (KB*KB)
#endif
#ifndef GB
    #define GB (KB*MB)
#endif

typedef int IO_HANDLE;

/***** Creating Machines *****/
enum io_status {
    IO_SUCCESS = 0,
    IO_ERROR,
    IO_COMPLETE,
    IO_IMPLEMENTATION=100,
};

// Function Types
typedef IO_HANDLE (*io_creator)(void*);
typedef void (*io_handle)(IO_HANDLE);
typedef int (*io_rw)(IO_HANDLE, void*, uint64_t*);
typedef struct io_desc *(*io_getter)(IO_HANDLE);


typedef struct bw_machine {
    // Interface functions
    io_creator create;
    io_handle destroy;
    io_handle lock;
    io_handle unlock;
    io_handle stop;
    io_getter get_read_desc;
    io_getter get_write_desc;
    io_rw read;
    io_rw write;

    // String to identify this io machine
    char *name;

    // Used for Implementation-dependent memory allocation
    void *alloc;

    // Buffer size recommendation: Used by implementor for optimal buffer allocation
    uint64_t buf_size_rec;

    // Timeout metric
    uint64_t timeout;

    // Implementation-specific struct
    void *obj;     
} IOM;

// Generic struct for describing a machine input or output
struct io_desc {
    // Unique ID for descriptor
    const IO_HANDLE handle;

    // Indicates that the descriptor was stopped
    char disabled;

    // Used for Implementation-dependent memory allocation
    void *alloc;

    // Implementation-specific struct
    void *obj;

    struct io_desc *next;
};

typedef struct machine_desc_t {
    IO_HANDLE handle;         // Unique IO Handle
    IOM *machine;             // IO Machine
    POOL *pool;               // Memory management pool
    char in_use;              // Flag designates machine memory in use
    pthread_mutex_t lock;     // Mutex lock for this machine
    struct io_desc *io_read;  // IO read descriptor for this machine
    struct io_desc *io_write; // IO write descriptor for this machine
    void *next;

    // Implementation-specific 
    void *_impl;
} IO_DESC;

void machine_desc_acquire(IO_DESC *desc);
void machine_desc_release(IO_DESC *desc);
void machine_lock(IO_HANDLE h);
void machine_unlock(IO_HANDLE h);
struct io_desc *machine_get_read_desc(IO_HANDLE h);
struct io_desc *machine_get_write_desc(IO_HANDLE h);

void machine_register_desc(struct machine_desc_t *addme, IO_HANDLE *handle);
struct machine_desc_t *machine_get_desc(IO_HANDLE h);
void machine_destroy_desc(IO_HANDLE h);
int machine_desc_read(IO_HANDLE h, void *buf, uint64_t *len);
int machine_desc_write(IO_HANDLE h, void *buf, uint64_t *len);
void machine_disable_write(IO_HANDLE h);
void machine_disable_read(IO_HANDLE h);

/***** Using Machines *****/
IOM *machine_register(const char *name);
IOM *get_machine(const char *name);
int machine_desc_init(POOL *p, IOM *machine, IO_DESC *b);
const IOM *get_machine_ref(IO_HANDLE handle);
IO_HANDLE request_handle(IOM *machine);
void machine_cleanup();

#endif
