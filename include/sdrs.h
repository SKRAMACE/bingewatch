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

void sdrrx_allow_overruns(IO_HANDLE h);

IOM *get_soapy_rx_machine();
IO_HANDLE new_soapy_rx_machine(const char *id);
void soapy_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth);

int soapy_rx_set_freq(IO_HANDLE h, double freq);
int soapy_rx_set_samp_rate(IO_HANDLE h, double samp_rate);
int soapy_rx_set_bandwidth(IO_HANDLE h, double bandwidth);
int soapy_rx_set_ppm(IO_HANDLE h, double ppm);

/* LIME SDR SUPPORT */
extern const IOM *lime_rx_machine;

void lime_set_gains(IO_HANDLE h, float lna, float tia, float pga);
void lime_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth);
int lime_rx_set_freq(IO_HANDLE h, double freq);
int lime_rx_set_samp_rate(IO_HANDLE h, double samp_rate);
int lime_rx_set_bandwidth(IO_HANDLE h, double bandwidth);
int lime_rx_set_ppm(IO_HANDLE h, double ppm);
IO_HANDLE new_lime_rx_machine();

/* UHD SDR SUPPORT */
extern const IOM *uhd_rx_machine;
IOM * get_uhd_rx_machine();
IO_HANDLE new_uhd_rx_machine(int channel);
IO_HANDLE new_uhd_rx_machine_devstr(int channel, char *devstr);
void uhd_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth);

void uhd_rx_set_gain(IO_HANDLE h, float lna);
int uhd_rx_set_freq(IO_HANDLE h, double freq);
int uhd_rx_set_samp_rate(IO_HANDLE h, double samp_rate);
int uhd_rx_set_bandwidth(IO_HANDLE h, double bandwidth);

/* B210 SDR SUPPORT */
extern const IOM *b210_rx_machine;
IO_HANDLE new_b210_rx_machine();
IO_HANDLE new_b210_rx_machine_devstr(char *devstr);
void b210_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth);

void b210_rx_set_gain(IO_HANDLE h, float lna);
int b210_rx_set_freq(IO_HANDLE h, double freq);
int b210_rx_set_samp_rate(IO_HANDLE h, double samp_rate);
int b210_rx_set_bandwidth(IO_HANDLE h, double bandwidth);

#endif
