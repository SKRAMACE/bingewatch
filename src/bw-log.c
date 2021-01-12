#include <envex.h>

#include "logging.h"
#include "bw-log.h"

int is_bingewatch_logging_init = 0;

void
strncpy_upper(char *dst, int n, char *src)
{
    unsigned char *a = (unsigned char *)src;
    unsigned char *b = (unsigned char *)dst;

    int c = 0;
    while (*a && (c++ < n)) {
        if (*a > 0x60 && *a < 0x7B) {
            *b = *a - 0x20;
        } else {
            *b = *a;
        }
        a++;
        b++;
    }
    dst[n-1] = 0;
}

void
bw_init_logging()
{
    is_bingewatch_logging_init = 1;

    char lvl[64];
    ENVEX_COPY(lvl, 64, "BW_SEGMENT_LOG_LEVEL", "error");
    segment_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_STREAM_LOG_LEVEL", "error");
    stream_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_MACHINE_LOG_LEVEL", "error");
    machine_set_log_level(lvl);
    machine_mgmt_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_RB_LOG_LEVEL", "error");
    rb_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_FBB_LOG_LEVEL", "error");
    fbb_set_log_level(lvl);

    ENVEX_COPY(lvl, 64, "BW_BLB_LOG_LEVEL", "error");
    blb_set_log_level(lvl);
}
