#ifndef __BW_LOG_H__
#define __BW_LOG_H__

#include <logex-lib.h>

#define bw_set_log_level_str(x) {\
    char _bw_set_log_level_lvl[32]; \
    strncpy_upper(_bw_set_log_level_lvl, 32, x); \
    set_log_level_str(_bw_set_log_level_lvl);}

void strncpy_upper(char *dst, int n, char *src);

#endif
