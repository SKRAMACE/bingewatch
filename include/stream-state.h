#ifndef __STREAM_STATE__
#define __STREAM_STATE__

#define STREAM_STATE_PRINT(s) (\
(s == STREAM_INIT) ? "INIT" :(\
(s == STREAM_READY) ? "READY" :(\
(s == STREAM_RUNNING) ? "RUNNING" :(\
(s == STREAM_FINISHING) ? "FINISHING" :(\
(s == STREAM_DONE) ? "DONE" :(\
(s == STREAM_STOPPED) ? "STOPPED" :(\
(s == STREAM_ERROR) ? "ERROR" : "UNKNOWN STATE")))))))

enum stream_state_e {
    STREAM_INIT,
    STREAM_READY,
    STREAM_RUNNING,
    STREAM_FINISHING,
    STREAM_DONE,
    STREAM_STOPPED,
    STREAM_ERROR,
};

#define STREAM_IS_RUNNING(s) ((s > STREAM_INIT && s < STREAM_DONE) ? 1 : 0)
#define STREAM_IS_PROCESSING(s) ((s == STREAM_RUNNING || s == STREAM_FINISHING) ? 1 : 0)

#endif
