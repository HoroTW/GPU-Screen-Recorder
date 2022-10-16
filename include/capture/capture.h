#ifndef GSR_CAPTURE_CAPTURE_H
#define GSR_CAPTURE_CAPTURE_H

#include <stdbool.h>

typedef struct AVCodecContext AVCodecContext;
typedef struct AVFrame AVFrame;

typedef struct gsr_capture gsr_capture;

struct gsr_capture {
    /* These methods should not be called manually. Call gsr_capture_* instead */
    int (*start)(gsr_capture *cap, AVCodecContext *video_codec_context);
    void (*tick)(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame); /* can be NULL */
    bool (*should_stop)(gsr_capture *cap, bool *err); /* can be NULL */
    int (*capture)(gsr_capture *cap, AVFrame *frame);
    void (*destroy)(gsr_capture *cap, AVCodecContext *video_codec_context);

    void *priv; /* can be NULL */
    bool started;
};

int gsr_capture_start(gsr_capture *cap, AVCodecContext *video_codec_context);
void gsr_capture_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame);
bool gsr_capture_should_stop(gsr_capture *cap, bool *err);
int gsr_capture_capture(gsr_capture *cap, AVFrame *frame);
/* Calls |gsr_capture_stop| as well */
void gsr_capture_destroy(gsr_capture *cap, AVCodecContext *video_codec_context);

#endif /* GSR_CAPTURE_CAPTURE_H */
