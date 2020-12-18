#include <stdio.h>

void
double_fmt(char *buf, int n, double v)
{
    double vd = (double)v;

    int f10 = 0;
    while (vd > 1000.0) {
        vd /= 1024;
        f10++;
    }
    snprintf(buf, n-1, "%.2f%s", vd,
        (f10 == 0) ? "" :
        (f10 == 1) ? "K" :
        (f10 == 2) ? "M" :
        (f10 == 3) ? "G" :
        (f10 == 4) ? "T" :
        (f10 == 5) ? "P" : "??");
    buf[n-1] = 0;
}

void
size_t_fmt(char *buf, int n, size_t v)
{
    double_fmt(buf, n, (double)v);
}
