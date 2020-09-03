#ifndef __SDRS_H__
#define __SDRS_H__

#ifdef BINGEWATCH_LOCAL
#include "machine.h"
#else
#include <bingewatch/machine.h>
#endif

/* SOAPY SDR SUPPORT */
enum supported_soapy_types_e {
    SDR_TYPE_LIME,
};

struct soapy_args_t {
    char id_str[128];
};

const IOM * get_soapy_rx_machine();
IO_HANDLE new_soapy_rx_machine();
void soapy_set_gains(IO_HANDLE h, float lna, float tia, float pga);
void soapy_set_rx(IO_HANDLE h, float freq, float rate, float bandwidth);

#endif
