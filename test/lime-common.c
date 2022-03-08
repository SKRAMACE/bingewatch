#include "lime-test.h"
#include "sdrs.h"

static struct lime_params_t defaults = {
    .freq       = 1000000000.0,
    .samp_rate  = 30720000.0,
    .bandwidth  = 30720000.0
};

static inline IO_HANDLE
lime_setup_vars(double freq, double samp_rate, double bandwidth)
{
    IO_HANDLE lime = new_lime_rx_machine();
    lime_set_rx(lime, freq, samp_rate, bandwidth);
    return lime;
}

IO_HANDLE
lime_test_setup()
{
    return lime_setup_vars(defaults.freq, defaults.samp_rate, defaults.bandwidth);
}

void
lime_teardown(IO_HANDLE lime)
{
    if (lime > 0) {
        lime_rx_machine->stop(lime);
        lime_rx_machine->destroy(lime);
    }
}

struct lime_params_t *
lime_test_get_defaults()
{
    return &defaults;
}
