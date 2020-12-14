#ifndef __SIMPLE_FILTERS_H__
#define __SIMPLE_FILTERS_H__

#include <filter.h>

enum iq_data_type_e {
    IQ_UNSUPPORTED = -1,
    IQ_FC32,
    IQ_SC16,
    IQ_SC8,
};

// Standard Filters
int byte_counter(IO_FILTER_ARGS);
int dump_to_binfile(IO_FILTER_ARGS);
int byte_count_limiter(IO_FILTER_ARGS);

struct io_filter_t *create_byte_count_limit_filter(void *alloc, const char *name, size_t byte_limit);
struct io_filter_t *create_byte_counter_filter(void *alloc, const char *name, size_t bytes_per_sample);
struct io_filter_t *create_conversion_filter(void *alloc, const char *name, int from_fmt, int to_fmt, int data_precision);

#endif
