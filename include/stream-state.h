#ifndef __STREAM_STATE__
#define __STREAM_STATE__

#define STREAM_STATE_PRINT(s) (\
(s == STREAM_INIT) ? "INIT" :(\
(s == STREAM_READY) ? "READY" :(\
(s == STREAM_RUNNING) ? "RUNNING" :(\
(s == STREAM_FINISHING) ? "FINISHING" :(\
(s == STREAM_DONE) ? "DONE" :(\
(s == STREAM_ERROR) ? "ERROR" : "UNKNOWN STATE"))))))

enum stream_state_e {
    STREAM_INIT,
    STREAM_READY,
    STREAM_RUNNING,
    STREAM_FINISHING,
    STREAM_DONE,
    STREAM_ERROR,
};

#endif
