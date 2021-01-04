#ifndef __SOAPY_H__
#define __SOAPY_H__

#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>

#include "machine.h"
#include "sdr-machine.h"

enum soapy_antennas_e {
    LIME_MINI_LNAH,
};

struct soapy_channel_t {
    struct sdr_channel_t _sdr;
    SoapySDRDevice *sdr;
    SoapySDRStream *rx;
    enum soapy_antennas_e antenna;
    int error_counter;
    float tia_gain;
    float pga_gain;
    double expected_timestamp;
    double ns_per_sample;
};

struct soapy_channel_t * soapy_get_channel(IO_HANDLE h);

#endif
