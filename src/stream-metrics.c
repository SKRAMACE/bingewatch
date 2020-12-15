struct bitrate_t {
    size_t bytes;
    struct timeval t0;
    struct timeval t1;
};

struct avg_t {
    double total;
    double n;
};
