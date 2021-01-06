#ifndef __BINGEWATCH_MACHINE_H__
#define __BINGEWATCH_MACHINE_H__

#include <pthread.h>
#include <memex.h>
#include <sys/time.h>

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

enum bw_status {
    BW_SUCCESS=0,
    BW_ERROR,
};

/***** Creating Machines *****/
enum io_status {
    // Error
    IO_ERROR=-1,            // Generic error

    // Success
    IO_SUCCESS=0,           // Generic success
    IO_COMPLETE,            // Machine has completed it's operations
    IO_NODATA,              // No data was available
    IO_CONTINUE,            // Continue to next iteration
    IO_BREAK,               // Stop iterating

    // Custom Implementation
    IO_IMPL=100,
};

// Function Types
typedef IO_HANDLE (*io_creator)(void*);
typedef void (*io_handle)(IO_HANDLE);
typedef int (*io_rw)(IO_HANDLE, void*, size_t*);
typedef struct io_desc *(*io_getter)(IO_HANDLE);
typedef void *(*io_meta)(IO_HANDLE);

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
    io_meta metrics;

    // String to identify this io machine
    char *name;

    // Used for Implementation-dependent memory allocation
    void *alloc;

    // Timeout metric
    size_t timeout;

    // Implementation-specific struct
    void *obj;     
} IOM;

#define IO_DESC_STATE_PRINT(s) (\
(s == IO_DESC_ENABLED) ? "ENABLED" :(\
(s == IO_DESC_DISABLING) ? "DISABLING" :(\
(s == IO_DESC_DISABLED) ? "DISABLED" :(\
(s == IO_DESC_STOPPED) ? "STOPPED" :(\
(s == IO_DESC_ERROR) ? "ERROR" : "UNKNOWN STATE")))))

enum io_desc_state_e {
    IO_DESC_ENABLED,
    IO_DESC_DISABLING,
    IO_DESC_DISABLED,
    IO_DESC_STOPPED,
    IO_DESC_ERROR,
};

#define METRIC_HIST 60
typedef void (*metrics_call)(void*, size_t);
typedef struct io_metrics_data_t {
    pthread_mutex_t lock;
    metrics_call fn;
    int _timer_signal;

    size_t total_bytes;
    struct timeval t_start;
    struct timeval t_stop;

    int time_i;
    size_t bytes_last;
    size_t bytes[METRIC_HIST];
    struct timeval time[METRIC_HIST];

    double rate_1;
    double rate_10;

    void *__impl;
} IO_METRICS;

struct io_metrics_t {
    IO_METRICS in;
    IO_METRICS out;
};

// Generic struct for describing a machine input or output
struct io_desc {
    // Unique ID for descriptor
    const IO_HANDLE handle;

    // Descriptor state
    enum io_desc_state_e state;

    // Mutex lock for this descriptor
    pthread_mutex_t lock;

    // Data packet size
    size_t size;

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
    struct io_metrics_t *metrics;
    void *next;

    // Implementation-specific 
    void *_impl;
} IO_DESC;

void io_desc_set_state(struct machine_desc_t *d, struct io_desc *io, enum io_desc_state_e new_state);

void machine_desc_acquire(IO_DESC *desc);
void machine_desc_release(IO_DESC *desc);
void machine_lock(IO_HANDLE h);
void machine_unlock(IO_HANDLE h);
struct io_desc *machine_get_read_desc(IO_HANDLE h);
struct io_desc *machine_get_write_desc(IO_HANDLE h);
void machine_desc_set_read_size(IO_HANDLE h, size_t len);
void machine_desc_set_write_size(IO_HANDLE h, size_t len);
void machine_metrics_enable();

void machine_metrics_timer_start(size_t ms);
void machine_metrics_timer_stop();
struct io_metrics_t *machine_metrics_create(POOL *pool);

void machine_register_desc(struct machine_desc_t *addme, IO_HANDLE *handle);
struct machine_desc_t *machine_get_desc(IO_HANDLE h);
void machine_destroy_desc(IO_HANDLE h);
int machine_desc_read(IO_HANDLE h, void *buf, size_t *bytes);
int machine_desc_write(IO_HANDLE h, void *buf, size_t *bytes);
void machine_disable_write(IO_HANDLE h);
void machine_disable_read(IO_HANDLE h);
void machine_stop(IO_HANDLE h);
void *machine_metrics(IO_HANDLE h);

/***** Using Machines *****/
IOM *machine_register(const char *name);
IOM *get_machine(const char *name);
int machine_desc_init(POOL *p, IOM *machine, IO_DESC *b);
const IOM *get_machine_ref(IO_HANDLE handle);
IO_HANDLE request_handle(IOM *machine);
void machine_cleanup();

#endif
