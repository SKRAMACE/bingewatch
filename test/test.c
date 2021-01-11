#include <stdlib.h>
#include <stdio.h>
#include <uuid/uuid.h>
#include <sys/time.h>
#include <envex.h>
#include <memex.h>

#include "test.h"

#define LOGEX_TAG "BW-TEST"
#include <logex-std.h>

#define TEST_QUEUE_ALLOC_LEN 10

char *bw_test_rootdir;
static char *_rootdir = NULL;
static POOL *testpool = NULL;

struct test_t {
    test_fn fn;
    const char *name;
};
    
static struct test_queue_t {
    int n_test;
    int len;
    struct test_t *tests;
} *queue = NULL;

static void
fmt_rootdir(char *root, char *group, char *tag)
{
    if (!_rootdir) {
        _rootdir = palloc(testpool, 1024);
    }

    snprintf(_rootdir, 1024, "%s/%s-%s", root, group, tag);
    bw_test_rootdir = _rootdir;
}

size_t
fill_float_data(size_t len, float **data)
{
    size_t bytes = len * sizeof(float);
    float *x = palloc(testpool, bytes);

    int i = 0;
    for (; i < 100; i++) {
        x[i] = (float)i;
    }

    *data = x;
    return bytes;
}

int
test_setup()
{
    // Set log level
    char *log_level;
    ENVEX_TOSTR_UPPER(log_level, "BW_TEST_LOG_LEVEL", "info");
    set_log_level_str(log_level);

    // Set output directory
    char *root;
    ENVEX_TOSTR(root, "BW_TEST_ROOT", "/tmp");

    char *group;
    ENVEX_TOSTR(group, "BW_TEST_GROUP", "bingewatch-test");

    char tag[1024];
    if (ENVEX_EXISTS("BW_TEST_TEST")) {
        ENVEX_COPY(tag, 1024, "BW_TEST_TAG", "");

    } else if (ENVEX_EXISTS("BW_TEST_TIMESTAMP")) {
        struct timeval tv;
        gettimeofday(&tv, NULL);

        // Convert seconds to "struct tm" (compatible with strftime)
        time_t nowtime = tv.tv_sec;
        struct tm *nowtm = gmtime(&nowtime);

        char timestr[64];
        strftime(timestr, 64, "%Y%m%d%H%M%S", nowtm);
        snprintf(tag, 1024, "%s", timestr);

    } else {
        uuid_t binuuid;
        uuid_generate_random(binuuid);
        uuid_unparse(binuuid, tag);
    }

    testpool = create_pool();

    fmt_rootdir(root, group, tag);

    info("outdir: %s", _rootdir);
    return 0;
}

int
_test_add(test_fn test, const char *name)
{
    if (!queue) {
        queue = calloc(1, sizeof(struct test_queue_t));
    }

    if (queue->n_test >= queue->len) {
        queue->len += TEST_QUEUE_ALLOC_LEN;
        queue->tests = realloc(queue->tests, queue->len * sizeof(struct test_t));
    }

    struct test_t *tp = &queue->tests[queue->n_test++];
    tp->fn = test;
    tp->name = name;

    return 0;
}

int
test_run()
{
    int ret = 2;
    if (!queue) {
        error("No tests added");
        goto do_return;
    }

    int t = 0;
    for (; t < queue->n_test; t++) {
        char *result = "ERROR";

        if (queue->tests[t].fn() == 0) {
            result = "\e[1;32mPASSED\e[0m";
        } else {
            result = "\e[1;31mFAILED\e[0m";
        }

        info("%s: %s", queue->tests[t].name, result);
    }

do_return:
    return ret;
}

int
test_cleanup()
{
    free_pool(testpool);
    return 0;
}
