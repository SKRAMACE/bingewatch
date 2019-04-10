#ifndef __SDR_MACHINE_H__
#define __SDR_MACHINE_H__

#include <radpool.h>

enum sdr_chan_direction {
    SDR_CHAN_NOINIT,
    SDR_CHAN_RX,
    SDR_CHAN_TX,
};

struct sdr_device_t {
    char id;
    char init;
    void *hw;
    struct sdr_device_t *next;
    POOL *pool;
    IO_HANDLE *io_handles;
    char io_handle_count;
    void *__device_impl;      // This is a placeholder so every implementation can be casted to this type
};

struct sdr_channel_t;
typedef void (*sdr_chan_fn)(struct sdr_channel_t *);

struct sdr_channel_t {
    struct sdr_device_t *device;
    struct sdr_channel_t *next;

    sdr_chan_fn destroy_channel_impl;

    // DEPRECATED: KEEPING FOR REVERSE-COMPATIBILITY, BUT WILL BE REMOVED SOON
    void *chan;

    // Universal Parameters
    uint64_t freq;
    uint32_t sample_rate;
    uint32_t bandwidth;
    uint32_t time;
    uint32_t bytes_per_sample;
    uint32_t gain;
    uint8_t num;
    enum sdr_chan_direction direction;

    IO_HANDLE handle;          // IO Handle for this buffer
    IO_HANDLE io_data;         // IO Handle for this buffer
    char in_use;               // Flag designates skiq memory in use
    pthread_mutex_t lock;      // Mutex lock for this sidekiq channel 
    struct io_desc *io_rx;     // IO read descriptor for this buffer
    struct io_desc *io_tx;     // IO write descriptor for this buffer
    POOL *pool;                // Memory management pool
    void *__chan_impl;      // This is a placeholder so every implementation can be casted to this type
};

typedef void *(*sdr_init)(POOL *, void *);
typedef void (*sdr_set)(void *);
typedef void *(*filter_init)(struct sdr_channel_t *, sdr_init, sdr_init);

typedef struct sdr_machine {
    // Interface functions
    sdr_set set_vars;
    sdr_init device;
    sdr_init channel;
    sdr_init rx_filter;
    sdr_init tx_filter;
} SDR;

IO_HANDLE create_sdr(IOM *machine, void *arg);
void sdr_machine_register(IOM *machine);

void add_channel(struct sdr_channel_t *chan);
struct sdr_channel_t *get_channel(IO_HANDLE h);
struct sdr_channel_t *pop_channel(IO_HANDLE h);
void sdr_channel_lock(IO_HANDLE h);
void sdr_channel_unlock(IO_HANDLE h);

void add_device(struct sdr_device_t *dev);
struct sdr_device_t *get_device(char id);
void cleanup_device(struct sdr_device_t *device);

struct io_desc *sdr_get_tx_desc(IO_HANDLE h);
struct io_desc *sdr_get_rx_desc(IO_HANDLE h);

int sdr_write(IO_HANDLE h, void *buf, uint64_t *len);
int sdr_read(IO_HANDLE h, void *buf, uint64_t *len);

#endif
