#include "bw-log.h"

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
