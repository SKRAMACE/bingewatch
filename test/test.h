#ifndef __BW_TEST_H__
#define __BW_TEST_H__

typedef int (*test_fn)(void);

extern const char *bw_test_rootdir;

#define test_add(x) _test_add(x, #x)
int _test_add(test_fn test, const char *name);

int test_run();
int test_setup();
int test_cleanup();

#endif
