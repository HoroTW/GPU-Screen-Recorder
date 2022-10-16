#include "../../include/capture/capture.h"
#include <stdio.h>

int gsr_capture_start(gsr_capture *cap, AVCodecContext *video_codec_context) {
    if(cap->started)
        return -1;

    int res = cap->start(cap, video_codec_context);
    if(res == 0)
        cap->started = true;

    return res;
}

void gsr_capture_tick(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame **frame) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsp_capture_tick failed: the gsr capture has not been started\n");
        return;
    }

    if(cap->tick)
        cap->tick(cap, video_codec_context, frame);
}

bool gsr_capture_should_stop(gsr_capture *cap, bool *err) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsr_capture_should_stop failed: the gsr capture has not been started\n");
        return false;
    }

    if(!cap->should_stop)
        return false;

    return cap->should_stop(cap, err);
}

int gsr_capture_capture(gsr_capture *cap, AVFrame *frame) {
    if(!cap->started) {
        fprintf(stderr, "gsr error: gsr_capture_capture failed: the gsr capture has not been started\n");
        return -1;
    }
    return cap->capture(cap, frame);
}

void gsr_capture_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    cap->destroy(cap, video_codec_context);
}
