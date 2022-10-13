#include "../../include/capture/capture.h"

int gsr_capture_start(gsr_capture *cap) {
    return cap->start(cap);
}

void gsr_capture_stop(gsr_capture *cap) {
    cap->stop(cap);
}

int gsr_capture_capture(gsr_capture *cap, AVFrame *frame) {
    return cap->capture(cap, frame);
}

void gsr_capture_destroy(gsr_capture *cap) {
    cap->destroy(cap);
}
