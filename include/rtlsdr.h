#ifndef __RTLSDR_H__
#define __RTLSDR_H__

#include <rtl-sdr.h>

#include "machine.h"
#include "sdr-machine.h"

struct rtlsdr_channel_t {
    struct sdr_channel_t _sdr;

    rtlsdr_dev_t *sdr;
    int n_gain_vals;
    int *gain_vals;
    int gain_mode;
    int test_mode;
};

struct rtlsdr_channel_t *rtlsdr_get_channel(IO_HANDLE h);
void rtlsdr_set_log_level(char *level);

#endif
