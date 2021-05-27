#ifndef __SDR_MACHINE_H__
#define __SDR_MACHINE_H__

#include <memex.h>

struct sdr_channel_t;
struct sdr_device_t;

// API Function Types
typedef  void (*sdr_args)(POOL *, void *);
typedef  struct sdr_device_t *(*sdr_device_init)(POOL *, void *);
typedef  struct sdr_channel_t *(*sdr_channel_init)(POOL *, struct sdr_device_t *, void *);
typedef  struct io_filter_t *(*sdr_filter_init)(POOL *, struct sdr_channel_t *, struct sdr_device_t *);
typedef  void (*sdr_set)(IO_HANDLE, void *);
typedef  int (*sdr_rw)(struct sdr_channel_t *, void *, size_t *);
typedef  int (*sdr_ref)(struct sdr_channel_t *);

typedef struct sdr_api_t {
    sdr_args set_vars;
    sdr_device_init device;
    sdr_channel_init channel;
    sdr_filter_init rx_filter;
    sdr_filter_init tx_filter;
    sdr_rw hw_read;
    sdr_ref channel_set;
    sdr_ref channel_reset;
    sdr_ref channel_start;
    sdr_set set_freq;
    sdr_set set_rate;
    sdr_set set_gain;
    void *_impl;
} SDR_API;

typedef void (*sdr_chan_fn)(struct sdr_channel_t *);

enum sdr_chan_state_e {
    SDR_CHAN_NOINIT,
    SDR_CHAN_RESET,
    SDR_CHAN_READY,
    SDR_CHAN_ERROR,
};

enum sdr_chan_mode_e {
    SDR_MODE_UNBUFFERED,
    SDR_MODE_BUFFERED,
    SDR_MODE_ERROR,
};

struct sdr_channel_t {
    IO_DESC _d;

    struct sdr_device_t *device;

    double freq;
    double rate;
    double bandwidth;
    double ppm;
    float gain;

    enum sdr_chan_state_e state;
    enum sdr_chan_mode_e mode;

    void *buffer;
    void *obj;
    size_t counter;

    int allow_overruns;

    sdr_chan_fn destroy_channel_impl;

    void *_impl;
};

typedef void (*sdr_device_fn)(struct sdr_device_t *);

struct sdr_device_t {
    uint32_t id;
    IO_HANDLE handle;
    pthread_mutex_t lock;

    POOL *pool;

    void *hw;

    struct sdr_channel_t **channels;
    int n_chan;
    int chan_len;

    sdr_device_fn destroy_device_impl;
    struct sdr_device_t *next;
    void *_impl;
};

void sdr_init_machine_functions(IOM *machine);
void sdr_init_api_functions(IOM *machine, SDR_API *api);
IO_HANDLE sdr_create(IOM *machine, void *arg);
int sdr_read_from_counter(struct sdr_channel_t *sdr, void *buf, size_t *n_samp);

#endif
