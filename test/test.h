#ifndef __BW_TEST_H__
#define __BW_TEST_H__

#include <envex.h>

typedef int (*test_fn)(void);

extern char *bw_test_rootdir;

#define test_init_logging() {\
    char *__bw_test_log_level; \
    ENVEX_TOSTR_UPPER(__bw_test_log_level, "BW_UNIT_TEST_LOG_LEVEL", "error"); \
    set_log_level_str(__bw_test_log_level); \
}

#define test_add(x) _test_add(x, #x)
int _test_add(test_fn test, const char *name);

size_t fill_float_data(size_t len, float **data);

int test_run();
int test_setup();
int test_cleanup();

#endif
