#ifndef __STREAM_METRICS_H__
#define __STREAM_METRICS_H__

struct stream_metric_t {
    size_t request_count;
    size_t response_count;

    size_t received_bytes;
    size_t received_count;

    size_t short_bytes;
    size_t short_count;
};

#endif
