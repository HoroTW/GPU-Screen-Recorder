#include "../include/sound.hpp"
extern "C" {
#include "../include/time.h"
}

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

        if(!p->read_data) {
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
                p->read_data = NULL;
                if(pa_stream_drop(p->stream) != 0)
                    goto fail;

                CHECK_DEAD_GOTO(p, rerror, fail);
                continue;
            }
        }

        const size_t space_free_in_output_buffer = p->output_length - p->output_index;
        if(space_free_in_output_buffer < p->read_length) {
            memcpy(p->output_data + p->output_index, (const uint8_t*)p->read_data + p->read_index, space_free_in_output_buffer);
            p->output_index = 0;
            p->read_index += space_free_in_output_buffer;
            p->read_length -= space_free_in_output_buffer;
            break;
        } else {
            memcpy(p->output_data + p->output_index, (const uint8_t*)p->read_data + p->read_index, p->read_length);
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

static pa_sample_format_t audio_format_to_pulse_audio_format(AudioFormat audio_format) {
    switch(audio_format) {
        case S16: return PA_SAMPLE_S16LE;
        case S32: return PA_SAMPLE_S32LE;
        case F32: return PA_SAMPLE_FLOAT32LE;
    }
    assert(false);
    return PA_SAMPLE_S16LE;
}

static int audio_format_to_get_bytes_per_sample(AudioFormat audio_format) {
    switch(audio_format) {
        case S16: return 2;
        case S32: return 4;
        case F32: return 4;
    }
    assert(false);
    return 2;
}

int sound_device_get_by_name(SoundDevice *device, const char *device_name, const char *description, unsigned int num_channels, unsigned int period_frame_size, AudioFormat audio_format) {
    pa_sample_spec ss;
    ss.format = audio_format_to_pulse_audio_format(audio_format);
    ss.rate = 48000;
    ss.channels = num_channels;

    pa_buffer_attr buffer_attr;
    buffer_attr.tlength = -1;
    buffer_attr.prebuf = -1;
    buffer_attr.minreq = -1;
    buffer_attr.maxlength = period_frame_size * audio_format_to_get_bytes_per_sample(audio_format) * num_channels; // 2/4 bytes/sample, @num_channels channels
    buffer_attr.fragsize = buffer_attr.maxlength;

    int error = 0;
    pa_handle *handle = pa_sound_device_new(nullptr, description, device_name, description, &ss, &buffer_attr, &error);
    if(!handle) {
        fprintf(stderr, "pa_sound_device_new() failed: %s. Audio input device %s might not be valid\n", pa_strerror(error), description);
        return -1;
    }

    device->handle = handle;
    device->frames = period_frame_size;
    return 0;
}

void sound_device_close(SoundDevice *device) {
    if(device->handle)
        pa_sound_device_free((pa_handle*)device->handle);
    device->handle = NULL;
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
            break;
        }

        pa_mainloop_iterate(main_loop, 1, NULL);
    }

    pa_mainloop_free(main_loop);
    return inputs;
}
