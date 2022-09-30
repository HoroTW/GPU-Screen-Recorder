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
#include <cmath>
#include <time.h>

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

struct pa_handle {
    pa_context *context;
    pa_stream *stream;
    pa_mainloop *mainloop;

    const void *read_data;
    size_t read_index, read_length;

    uint8_t *output_data;
    size_t output_index, output_length;

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

    if (s->output_data) {
        free(s->output_data);
        s->output_data = NULL;
    }

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
    p->read_data = NULL;
    p->read_length = 0;
    p->read_index = 0;

    const int buffer_size = attr->maxlength;
    void *buffer = malloc(buffer_size);
    if(!buffer) {
        fprintf(stderr, "failed to allocate buffer for audio\n");
        *rerror = -1;
        return NULL;
    }

    p->output_data = (uint8_t*)buffer;
    p->output_length = buffer_size;
    p->output_index = 0;

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

// Returns a negative value on failure or if |p->output_length| data is not available within the time frame specified by the sample rate
static int pa_sound_device_read(pa_handle *p) {
    assert(p);

    const int64_t timeout_ms = std::round((1000.0 / (double)pa_stream_get_sample_spec(p->stream)->rate) * 1000.0);
    const double start_time = clock_get_monotonic_seconds();

    bool success = false;
    int r = 0;
    int *rerror = &r;
    CHECK_DEAD_GOTO(p, rerror, fail);

    while (p->output_index < p->output_length) {
        if((clock_get_monotonic_seconds() - start_time) * 1000 >= timeout_ms)
            return -1;

        if(p->read_data) {
            assert(p->output_index == 0);
            memcpy(p->output_data, (const uint8_t*)p->read_data + p->read_index, p->read_length);
            p->output_index += p->read_length;
            p->read_data = NULL;
            p->read_length = 0;
            p->read_index = 0;

            if(pa_stream_drop(p->stream) != 0)
                goto fail;
        }

        pa_mainloop_prepare(p->mainloop, 1 * 1000); // 1 ms
        pa_mainloop_poll(p->mainloop);
        pa_mainloop_dispatch(p->mainloop);

        if(pa_stream_peek(p->stream, &p->read_data, &p->read_length) < 0)
            goto fail;

        if(!p->read_data && p->read_length == 0)
            continue;

        if(!p->read_data && p->read_length > 0) {
            // There is a hole in the stream :( drop it. Maybe we should generate silence instead? TODO
            if(pa_stream_drop(p->stream) != 0)
                goto fail;
            continue;
        }

        if(p->read_length <= 0) {
            CHECK_DEAD_GOTO(p, rerror, fail);
            continue;
        }

        const size_t space_free_in_output_buffer = p->output_length - p->output_index;
        if(space_free_in_output_buffer < p->read_length) {
            assert(p->read_index == 0);
            memcpy(p->output_data + p->output_index, p->read_data, space_free_in_output_buffer);
            p->output_index = 0;
            p->read_index += space_free_in_output_buffer;
            p->read_length -= space_free_in_output_buffer;
            break;
        } else {
            assert(p->read_index == 0);
            memcpy(p->output_data + p->output_index, p->read_data, p->read_length);
            p->output_index += p->read_length;
            p->read_data = NULL;
            p->read_length = 0;
            p->read_index = 0;
            
            if(pa_stream_drop(p->stream) != 0)
                goto fail;

            if(p->output_index == p->output_length) {
                p->output_index = 0;
                break;
            }
        }
    }

    success = true;

    fail:
    return success ? 0 : -1;
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
    char stream_name[1024];
    snprintf(stream_name, sizeof(stream_name), "gpu-screen-recorder-%s", name);

    pa_handle *handle = pa_sound_device_new(nullptr, stream_name, name, stream_name, &ss, &buffer_attr, &error);
    if(!handle) {
        fprintf(stderr, "pa_sound_device_new() failed: %s. Audio input device %s might not be valid\n", pa_strerror(error), name);
        return -1;
    }

    device->handle = handle;
    device->frames = period_frame_size;
    return 0;
}

void sound_device_close(SoundDevice *device) {
    pa_sound_device_free((pa_handle*)device->handle);
}

int sound_device_read_next_chunk(SoundDevice *device, void **buffer) {
    pa_handle *pa = (pa_handle*)device->handle;
    if(pa_sound_device_read(pa) < 0) {
        //fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
        return -1;
    }
    *buffer = pa->output_data;
    return device->frames;
}

static void pa_state_cb(pa_context *c, void *userdata) {
    pa_context_state state = pa_context_get_state(c);
    int *pa_ready = (int*)userdata;
    switch(state) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
        default:
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            *pa_ready = 2;
            break;
        case PA_CONTEXT_READY:
            *pa_ready = 1;
            break;
    }
}

static void pa_sourcelist_cb(pa_context *ctx, const pa_source_info *source_info, int eol, void *userdata) {
    if(eol > 0)
        return;

    std::vector<AudioInput> *inputs = (std::vector<AudioInput>*)userdata;
    inputs->push_back({ source_info->name, source_info->description });
}

std::vector<AudioInput> get_pulseaudio_inputs() {
    std::vector<AudioInput> inputs;
    pa_mainloop *main_loop = pa_mainloop_new();

    pa_context *ctx = pa_context_new(pa_mainloop_get_api(main_loop), "gpu-screen-recorder");
    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
    int state = 0;
    int pa_ready = 0;
    pa_context_set_state_callback(ctx, pa_state_cb, &pa_ready);

    pa_operation *pa_op = NULL;

    for(;;) {
        // Not ready
        if(pa_ready == 0) {
            pa_mainloop_iterate(main_loop, 1, NULL);
            continue;
        }

        switch(state) {
            case 0: {
                pa_op = pa_context_get_source_info_list(ctx, pa_sourcelist_cb, &inputs);
                ++state;
                break;
            }
        }

        // Couldn't get connection to the server
        if(pa_ready == 2 || (state == 1 && pa_op && pa_operation_get_state(pa_op) == PA_OPERATION_DONE)) {
            if(pa_op)
                pa_operation_unref(pa_op);
            pa_context_disconnect(ctx);
            pa_context_unref(ctx);
            pa_mainloop_free(main_loop);
            return inputs;
        }

        pa_mainloop_iterate(main_loop, 1, NULL);
    }

    pa_mainloop_free(main_loop);
}