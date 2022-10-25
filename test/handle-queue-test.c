#include <testex.h>

#include "simple-buffers.h"
#include "logging.h"

#define LOGEX_TAG "HQ-TEST"
#include <logex-main.h>

int
basic_test()
{
    int ret = TESTEX_FAILURE;
    POOL *pool = create_pool();

    uint32_t n_samp = 10;
    size_t d_bytes = n_samp * sizeof(int);
    int *data_pos = palloc(pool, d_bytes);
    int *data_neg = palloc(pool, d_bytes);
    for (int i = 0; i < n_samp; i++) {
        int v = i + 1;
        data_pos[i] = v;
        data_neg[i] = -v;
    }

    IO_HANDLE h = new_hq_machine();

    HQ_ENTRY e;
    size_t bytes = sizeof(HQ_ENTRY);
    ASSERT_SUCCESS(hq_machine->read(h, &e, &bytes));
    ASSERT_EQUAL(bytes, 0);

    bytes = d_bytes;
    ASSERT_SUCCESS(hq_machine->write(h, data_pos, &bytes));
    ASSERT_SUCCESS(hq_machine->write(h, data_neg, &bytes));

    HQ_ENTRY e_pos, e_neg;
    bytes = sizeof(HQ_ENTRY);
    hq_machine->read(h, &e_pos, &bytes);

    bytes = sizeof(HQ_ENTRY);
    hq_machine->read(h, &e_neg, &bytes);

    bytes = sizeof(HQ_ENTRY);
    hq_machine->read(h, &e, &bytes);
    ASSERT_EQUAL(bytes, 0);

    ASSERT_EQUAL(e_pos.bytes, d_bytes);
    ASSERT_EQUAL(e_neg.bytes, d_bytes);

    int *ep = (int *)e_pos.buf;
    int *en = (int *)e_neg.buf;
    ASSERT_ARRAY_EQUAL(data_pos, ep, n_samp);
    ASSERT_ARRAY_EQUAL(data_neg, en, n_samp);
    ret = TESTEX_SUCCESS;

testex_return:
    free_pool(pool);
    return ret;
}

int
main(int nargs, char *argv[])
{
    TESTEX_LOG_INIT("info");
    testex_setup();

    testex_add(basic_test);

    testex_run();
    testex_cleanup();
}
