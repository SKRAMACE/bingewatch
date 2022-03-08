#ifndef __LIME_TEST_H__
#define __LIME_TEST_H__

#include "machine.h"

struct lime_params_t {
    double freq;
    double samp_rate;
    double bandwidth;
};

struct lime_params_t *lime_test_get_defaults();
IO_HANDLE lime_test_setup();
void lime_teardown(IO_HANDLE lime);

#endif
