#ifndef __SDRS_H__
#define __SDRS_H__

#ifdef BINGEWATCH_LOCAL
#include "machine.h"
#include "filter.h"
#else
#include <bingewatch/machine.h>
#include <bingewatch/filter.h>
#endif

/* GENERIC SDR SUPPORT */
int sdrrx_read(IO_FILTER_ARGS);
void sdrrx_enable_buffering(IO_HANDLE h, size_t n_samp, size_t n_block);
void sdrrx_enable_buffering_rate(IO_HANDLE h, double rate);
void sdrrx_get_buffer_info(IO_HANDLE h, size_t *size, size_t *bytes);
void sdrrx_allow_overruns(IO_HANDLE h);
int sdrrx_reset(IO_HANDLE h);

/* SOAPY SDR SUPPORT */
extern const IOM *soapy_rx_machine;

enum supported_soapy_types_e {
    SDR_TYPE_LIME,
};

struct soapy_args_t {
    char id_str[128];
};

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

/* RTL SDR SUPPORT */
extern const IOM *rtlsdr_rx_machine;
IOM *get_rtlsdr_rx_machine();
IO_HANDLE new_rtlsdr_rx_machine(const char *id);
void rtlsdr_set_rx(IO_HANDLE h, double freq, double rate, double bandwidth);

void rtlsdr_rx_set_gain(IO_HANDLE h, float lna);
int rtlsdr_rx_set_freq(IO_HANDLE h, double freq);
int rtlsdr_rx_set_samp_rate(IO_HANDLE h, double samp_rate);
int rtlsdr_rx_set_bandwidth(IO_HANDLE h, double bandwidth);
int rtlsdr_rx_set_ppm(IO_HANDLE h, double ppm);
int rtlsdr_rx_set_testmode(IO_HANDLE h);

#endif
