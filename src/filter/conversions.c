#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "machine.h"
#include "filter.h"
#include "simple-filters.h"

//TODO: set_precision()
#define DEFAULT_PRECISION 12

typedef struct conversion_buf_t {
    char *buf;
    size_t len;
    int precision;
} CBUF;

typedef struct fc32_t {
    float i;
    float q;
} fc32;

typedef struct sc16_t {
    short i;
    short q;
} sc16;

#define DECLARE_CONVERT_FNS(fmt) \
static void fmt##_to_double(double *d, void *x, size_t *bytes) { \
    fmt *src = (fmt *)x; \
    double *dst = d; \
    size_t remaining = *bytes; \
    size_t total = 0; \
    while (remaining >= sizeof(fmt)) { \
        *d++ = (double)(*src++); \
        remaining -= sizeof(fmt); \
        total += sizeof(double); \
    } \
*bytes = total; \
} \
static void double_to_##fmt(double *d, void *x, size_t *bytes) { \
    double *src = d; \
    fmt *dst = (fmt *)x; \
    size_t remaining = *bytes; \
    size_t total = 0; \
    while (remaining >= sizeof(double)) { \
        *dst++ = (fmt)(*d++); \
        remaining -= sizeof(double); \
        total += sizeof(fmt); \
    } \
*bytes = total; \
}

#define CONVERT_TO(x) double_to_##x
#define CONVERT_FROM(x) x##_to_double

DECLARE_CONVERT_FNS(float)
DECLARE_CONVERT_FNS(short)
DECLARE_CONVERT_FNS(char)

typedef void (*convert_copy)(double *, void *, size_t *);

typedef struct iq_data_type_desc_t {
    char name[64];
    enum iq_data_type_e type;
    char data_size;
    char is_float;
    convert_copy pull;
    convert_copy push;
} IQ_DATATYPE;

static IQ_DATATYPE iq_desc[] = {
    {"float complex32",  IQ_FC32, 4, 1, CONVERT_FROM(float), CONVERT_TO(float)},
    {"signed complex16", IQ_SC16, 2, 0, CONVERT_FROM(short), CONVERT_TO(short)},
    {"signed complex8",  IQ_SC8,  1, 0, CONVERT_FROM(char),  CONVERT_TO(char)},
    {"UNSUPPORTED",  IQ_UNSUPPORTED,  0, 0, NULL, NULL},
};

typedef struct generic_conversion_buf_t {
    char *buf;
    double *tmp;
    int precision;
    struct iq_data_type_desc_t *from;
    struct iq_data_type_desc_t *to;
} GCB;

static void
_scale_float_to_integer(char precision, double *buf, size_t *len)
{
    double *d = buf;
    size_t remaining = *len;

    float scale_factor = (1 << (precision - 1)) - 1;

    if (scale_factor == 1) {
        return;
    }

    while (remaining >= sizeof(double)) {
        double ds = *d;
        *d = ds * scale_factor;
        d++;
        remaining -= sizeof(double);
    }
}

static void
_scale_integer_to_float(char precision, double *buf, size_t *len)
{
    double *d = buf;
    size_t remaining = *len;

    float scale_factor = (1 << (precision - 1)) - 1;

    if (scale_factor == 1) {
        return;
    }

    while (remaining >= sizeof(double)) {
        double ds = *d;
        *d = ds / scale_factor;
        d++;
        remaining -= sizeof(double);
    }
}

static int
iq_type_conversion(IO_FILTER_ARGS)
{
    IOF_DISABLED();
    int ret = IO_ERROR;

    GCB *b = (GCB *)IO_FILTER_ARGS_FILTER->obj;

    // Verify that filter was properly initialized
    if (!b) {
        ret = IO_ERROR;
    }

    // Calculate difference in data size
    char max = (b->to->data_size > b->from->data_size) ? b->to->data_size : b->from->data_size;
    char min = (b->to->data_size < b->from->data_size) ? b->to->data_size : b->from->data_size;
    char ratio = max / min;

    // Resize the buffer to support the larger format
    size_t buf_len = *IO_FILTER_ARGS_BYTES * ratio;
    b->buf = repalloc(b->buf, buf_len, IO_FILTER_ARGS_FILTER->alloc);

    // Resize the intermediary buffer for the amount of data
    size_t tmp_len = *IO_FILTER_ARGS_BYTES * sizeof(double);
    b->tmp = repalloc(b->tmp, tmp_len, IO_FILTER_ARGS_FILTER->alloc);

    // Call conversion function with proper ordering and direction
    switch (IO_FILTER_ARGS_FILTER->direction) {

    // Data "x" comes from the previous (to) filter
    case IOF_WRITE:
        // Convert filter buffer before sending to next filter
        tmp_len = *IO_FILTER_ARGS_BYTES;
        b->from->pull(b->tmp, IO_FILTER_ARGS_BUF, &tmp_len);

        // Scale float [-1, 1] to integer [2^(N-1)-1, 2^(N-1)-1] and visa versa
        if (b->to->is_float ^ b->from->is_float) {
            if (b->to->is_float) {
                _scale_integer_to_float(b->precision, b->tmp, &tmp_len);
            } else {
                _scale_float_to_integer(b->precision, b->tmp, &tmp_len);
            }
        }

        b->to->push(b->tmp, b->buf, &tmp_len);

        ret = CALL_NEXT_FILTER_ARGS(b->buf, &tmp_len, IO_FILTER_ARGS_BLOCK, b->to->data_size);

        // Scale "bytes processed" variable
        if (b->to->data_size > b->from->data_size) {
            *IO_FILTER_ARGS_BYTES = tmp_len / ratio;
        } else if (b->to->data_size < b->from->data_size) {
            *IO_FILTER_ARGS_BYTES = tmp_len * ratio;
        }
        break;

    // Data "x" comes from the next (from) filter
    case IOF_READ:
        // Scale "buffer size" variable.
        // (This is to avoid needing to buffer when the conversion increases the sample size)
        if (b->to->data_size > b->from->data_size) {
            *IO_FILTER_ARGS_BYTES /= ratio;
        } else if (b->to->data_size < b->from->data_size) {
            *IO_FILTER_ARGS_BYTES *= ratio;
        }

        ret = CALL_NEXT_FILTER_ARGS(b->buf, IO_FILTER_ARGS_BYTES, IO_FILTER_ARGS_BLOCK, b->from->data_size);

        // Convert filter buffer before sending to next filter
        tmp_len = *IO_FILTER_ARGS_BYTES;
        b->from->pull(b->tmp, b->buf, &tmp_len);

        // Scale float [-1, 1] to integer [2^(N-1)-1, 2^(N-1)-1] and visa versa
        if (b->to->is_float ^ b->from->is_float) {
            if (b->to->is_float) {
                _scale_integer_to_float(b->precision, b->tmp, &tmp_len);
            } else {
                _scale_float_to_integer(b->precision, b->tmp, &tmp_len);
            }
        }

        b->to->push(b->tmp, IO_FILTER_ARGS_BUF, &tmp_len);
        *IO_FILTER_ARGS_BYTES = tmp_len;
        break;

    case IOF_BIDIRECTIONAL:
        ERROR_DIRECTION();
        ret = IO_ERROR;
        break;

    default:
        ret = IO_ERROR;
    }
    return ret;
}

static int
get_iq_desc(int type, IQ_DATATYPE **iq)
{
    IQ_DATATYPE *desc = iq_desc;
    while (desc->type != IQ_UNSUPPORTED) {
        if (desc->type == type) {
            *iq = desc;
            return IO_SUCCESS;
        }
        desc++;
    }
    *iq = NULL;
    return IO_ERROR;
}

struct io_filter_t *
create_conversion_filter(void *alloc, const char *name, int from_fmt, int to_fmt, int data_precision)
{
    struct io_filter_t *f = create_filter(alloc, name, iq_type_conversion);
    GCB *desc = palloc(alloc, sizeof(GCB));
    desc->buf = NULL;
    desc->tmp = NULL;
    desc->precision = data_precision;

    if (get_iq_desc(from_fmt, &desc->from) != IO_SUCCESS) {
        fprintf(stderr, "ERROR: Unsupported IQ Datatype (%d)\n", from_fmt);
        return NULL;
    }

    if (get_iq_desc(to_fmt, &desc->to) != IO_SUCCESS) {
        fprintf(stderr, "ERROR: Unsupported IQ Datatype (%d)\n", to_fmt);
        return NULL;
    }

    f->obj = desc;

    return f;
}
