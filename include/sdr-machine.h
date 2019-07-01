#ifndef __SDR_MACHINE_H__
#define __SDR_MACHINE_H__

#include <radpool.h>

enum sdr_chan_direction_e {
    SDR_CHAN_NOINIT,
    SDR_CHAN_RX,
    SDR_CHAN_TX,
};

struct sdr_channel_t;
struct sdr_device_t;

// API Function Types
typedef  void (*sdr_args)(POOL *, void *);
typedef  struct sdr_device_t *(*sdr_device_init)(POOL *, void *);
typedef  struct sdr_channel_t *(*sdr_channel_init)(POOL *, struct sdr_device_t *, void *);
typedef  struct io_filter_t *(*sdr_filter_init)(POOL *, struct sdr_channel_t *, struct sdr_device_t *);
typedef  void (*sdr_set)(IO_HANDLE, void *);

typedef struct sdr_api_t {
    sdr_args set_vars;
    sdr_device_init device;
    sdr_channel_init channel;
    sdr_filter_init rx_filter;
    sdr_filter_init tx_filter;
    sdr_set set_freq;
    sdr_set set_rate;
    sdr_set set_gain;
    void *_impl;
} SDR_API;

typedef void (*sdr_chan_fn)(struct sdr_channel_t *);

struct sdr_channel_t {
    POOL *pool;
    IO_HANDLE handle;
    char in_use;

    float freq;
    float rate;
    float bandwidth;
    float gain;
    enum sdr_chan_direction_e dir;
    char init;
    struct io_desc *io;
    pthread_mutex_t lock;

    sdr_chan_fn destroy_channel_impl;

    struct sdr_channel_t *next;

    void *_impl;
};

typedef void (*sdr_device_fn)(struct sdr_device_t *);

struct sdr_device_t {
    uint32_t id;
    IO_HANDLE handle;
    pthread_mutex_t lock;

    POOL *pool;

    void *hw;
    struct sdr_channel_t *channels;
    sdr_device_fn destroy_device_impl;
    struct sdr_device_t *next;
    void *_impl;
};

void sdr_init_machine_functions(IOM *machine);
void sdr_init_api_functions(IOM *machine, SDR_API *api);
const struct sdr_device_t *sdr_get_device(IO_HANDLE h);
struct sdr_channel_t *sdr_get_channel(IO_HANDLE h, const struct sdr_device_t *dev);
IO_HANDLE sdr_create(const IOM *machine, void *arg);

#endif
