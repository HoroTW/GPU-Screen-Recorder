#ifndef GSR_CAPTURE_CAPTURE_H
#define GSR_CAPTURE_CAPTURE_H

#include <stdbool.h>

typedef struct AVFrame AVFrame;

typedef struct gsr_capture gsr_capture;

struct gsr_capture {
    int (*start)(gsr_capture *cap);
    void (*stop)(gsr_capture *cap);
    int (*capture)(gsr_capture *cap, AVFrame *frame);
    void (*destroy)(gsr_capture *cap);

    void *priv;
};

int gsr_capture_start(gsr_capture *cap);
void gsr_capture_stop(gsr_capture *cap);
int gsr_capture_capture(gsr_capture *cap, AVFrame *frame);
/* Calls |gsr_capture_stop| as well */
void gsr_capture_destroy(gsr_capture *cap);

#endif /* GSR_CAPTURE_CAPTURE_H */
