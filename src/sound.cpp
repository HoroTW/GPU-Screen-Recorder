/*
    Copyright (C) 2020 dec05eba

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "../include/sound.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <cmath>

#include <pulse/pulseaudio.h>
#include <pulse/mainloop.h>
#include <pulse/xmalloc.h>
#include <pulse/error.h>

#define CHECK_DEAD_GOTO(p, rerror, label)                               \
    do {                                                                \
        if (!(p)->context || !PA_CONTEXT_IS_GOOD(pa_context_get_state((p)->context)) || \
            !(p)->stream || !PA_STREAM_IS_GOOD(pa_stream_get_state((p)->stream))) { \
            if (((p)->context && pa_context_get_state((p)->context) == PA_CONTEXT_FAILED) || \
                ((p)->stream && pa_stream_get_state((p)->stream) == PA_STREAM_FAILED)) { \
                if (rerror)                                             \
                    *(rerror) = pa_context_errno((p)->context);         \
            } else                                                      \
                if (rerror)                                             \
                    *(rerror) = PA_ERR_BADSTATE;                        \
            goto label;                                                 \
        }                                                               \
    } while(false);

static double clock_get_monotonic_seconds() {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

static int sound_device_index = 0;

struct pa_handle {
    pa_context *context;
    pa_stream *stream;
    pa_mainloop *mainloop;

    const void *read_data;
    size_t read_index, read_length;

    int operation_success;
};

static void pa_sound_device_free(pa_handle *s) {
    assert(s);

    if (s->stream)
        pa_stream_unref(s->stream);

    if (s->context) {
        pa_context_disconnect(s->context);
        pa_context_unref(s->context);
    }

    if (s->mainloop)
        pa_mainloop_free(s->mainloop);

    pa_xfree(s);
}

static pa_handle* pa_sound_device_new(const char *server,
        const char *name,
        const char *dev,
        const char *stream_name,
        const pa_sample_spec *ss,
        const pa_buffer_attr *attr,
        int *rerror) {
    pa_handle *p;
    int error = PA_ERR_INTERNAL, r;

    p = pa_xnew0(pa_handle, 1);

    if (!(p->mainloop = pa_mainloop_new()))
        goto fail;

    if (!(p->context = pa_context_new(pa_mainloop_get_api(p->mainloop), name)))
        goto fail;

    if (pa_context_connect(p->context, server, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    for (;;) {
        pa_context_state_t state = pa_context_get_state(p->context);

        if (state == PA_CONTEXT_READY)
            break;

        if (!PA_CONTEXT_IS_GOOD(state)) {
            error = pa_context_errno(p->context);
            goto fail;
        }

        pa_mainloop_iterate(p->mainloop, 1, NULL);
    }

    if (!(p->stream = pa_stream_new(p->context, stream_name, ss, NULL))) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    r = pa_stream_connect_record(p->stream, dev, attr,
        (pa_stream_flags_t)(PA_STREAM_INTERPOLATE_TIMING|PA_STREAM_ADJUST_LATENCY|PA_STREAM_AUTO_TIMING_UPDATE));

    if (r < 0) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    for (;;) {
        pa_stream_state_t state = pa_stream_get_state(p->stream);

        if (state == PA_STREAM_READY)
            break;

        if (!PA_STREAM_IS_GOOD(state)) {
            error = pa_context_errno(p->context);
            goto fail;
        }

        pa_mainloop_iterate(p->mainloop, 1, NULL);
    }

    return p;

fail:
    if (rerror)
        *rerror = error;
    pa_sound_device_free(p);
    return NULL;
}

// Returns a negative value on failure. Always blocks a time specified matching the sampling rate of the audio.
static int pa_sound_device_read(pa_handle *p, void *data, size_t length) {
    assert(p);

    int r = 0;
    int *rerror = &r;
    bool retry = true;

    pa_mainloop_iterate(p->mainloop, 0, NULL);
    const int64_t timeout_ms = std::round((1000.0 / (double)pa_stream_get_sample_spec(p->stream)->rate) * 1000.0);

    CHECK_DEAD_GOTO(p, rerror, fail);

    while(true) {
        if(pa_stream_readable_size(p->stream) < length) {
            if(!retry)
                break;

            retry = false;

            const double start_time = clock_get_monotonic_seconds();
            while((clock_get_monotonic_seconds() - start_time) * 1000.0 < timeout_ms) {
                pa_mainloop_prepare(p->mainloop, 1 * 1000);
                pa_mainloop_poll(p->mainloop);
                pa_mainloop_dispatch(p->mainloop);
            }

            continue;
        }

        r = pa_stream_peek(p->stream, &p->read_data, &p->read_length);
        if(r != 0) {
            if(retry)
                usleep(timeout_ms * 1000);
            return -1;
        }

        if(p->read_length < length || !p->read_data) {
            pa_stream_drop(p->stream);
            if(retry)
                usleep(timeout_ms * 1000);
            return -1;
        }

        memcpy(data, p->read_data, length);
        pa_stream_drop(p->stream);
        return 0;
    }

    fail:
    return -1;
}

int sound_device_get_by_name(SoundDevice *device, const char *name, unsigned int num_channels, unsigned int period_frame_size) {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 48000;
    ss.channels = num_channels;
    int error;

    pa_buffer_attr buffer_attr;
    buffer_attr.tlength = -1;
    buffer_attr.prebuf = -1;
    buffer_attr.minreq = -1;
    buffer_attr.maxlength = period_frame_size * 2 * num_channels; // 2 bytes/sample, @num_channels channels
    buffer_attr.fragsize = buffer_attr.maxlength;

    // We want a unique stream name for every device which allows each input to be a different box in pipewire graph software
    char stream_name[64];
    snprintf(stream_name, sizeof(stream_name), "record-%d", sound_device_index);
    ++sound_device_index;

    pa_handle *handle = pa_sound_device_new(nullptr, "gpu-screen-recorder", name, stream_name, &ss, &buffer_attr, &error);
    if(!handle) {
        fprintf(stderr, "pa_simple_new() failed: %s. Audio input device %s might not be valid\n", pa_strerror(error), name);
        return -1;
    }

    int buffer_size = buffer_attr.maxlength;
    void *buffer = malloc(buffer_size);
    if(!buffer) {
        fprintf(stderr, "failed to allocate buffer for audio\n");
        pa_sound_device_free(handle);
        return -1;
    }

    fprintf(stderr, "Using pulseaudio\n");

    device->handle = handle;
    device->buffer = buffer;
    device->buffer_size = buffer_size;
    device->frames = period_frame_size;
    return 0;
}

void sound_device_close(SoundDevice *device) {
    pa_sound_device_free((pa_handle*)device->handle);
    free(device->buffer);
}

int sound_device_read_next_chunk(SoundDevice *device, void **buffer) {
    if(pa_sound_device_read((pa_handle*)device->handle, device->buffer, device->buffer_size) < 0) {
        //fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
        return -1;
    }
    *buffer = device->buffer;
    return device->frames;
}