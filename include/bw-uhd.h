#ifndef __BW_UHD_H__
#define __BW_UHD_H__

#include <uhd.h>

#include "machine.h"
#include "sdr-machine.h"

struct uhd_channel_t {
    struct sdr_channel_t _sdr;
    uhd_usrp_handle *sdr;
    size_t channel;
    uhd_tune_request_t tune_request;
    uhd_tune_result_t tune_result;
    uhd_stream_args_t stream_args;
    uhd_stream_cmd_t stream_cmd;
    uhd_rx_streamer_handle rx_streamer;
    uhd_rx_metadata_handle rx_metadata;
    size_t max_samples;
    int error_counter;
};

struct uhd_channel_t *uhd_get_channel(IO_HANDLE h);

#endif
