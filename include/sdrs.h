#ifndef __SDRS_H__
#define __SDRS_H__

#ifdef BINGEWATCH_LOCAL
#include "machine.h"
#else
#include <bingewatch/machine.h>
#endif

/* SOAPY SDR SUPPORT */
extern const IOM *soapy_rx_machine;

enum supported_soapy_types_e {
    SDR_TYPE_LIME,
};

struct soapy_args_t {
    char id_str[128];
};

const IOM * get_soapy_rx_machine();
IO_HANDLE new_soapy_rx_machine();
void soapy_set_gains(IO_HANDLE h, float lna, float tia, float pga);
void soapy_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth);

int soapy_rx_set_freq(IO_HANDLE h, double freq);
int soapy_rx_set_samp_rate(IO_HANDLE h, double samp_rate);
int soapy_rx_set_bandwidth(IO_HANDLE h, double bandwidth);

#endif
