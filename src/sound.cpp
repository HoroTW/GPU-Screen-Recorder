#include "../include/sound.hpp"

#include <stdlib.h>
#include <stdio.h>

#define ALSA_PCM_NEW_HW_PARAMS_API
#include <alsa/asoundlib.h>

int sound_device_get_by_name(SoundDevice *device, const char *name, unsigned int num_channels, unsigned int period_frame_size) {
    int rc;
    snd_pcm_t *handle;

    rc = snd_pcm_open(&handle, name, SND_PCM_STREAM_CAPTURE, 0);
    if(rc < 0) {
        fprintf(stderr, "unable to open pcm device 'default', reason: %s\n", snd_strerror(rc));
        return rc;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    // Fill the params with default values
    snd_pcm_hw_params_any(handle, params);
    // Interleaved mode
    snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    // Signed 16--bit little-endian format
    snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(handle, params, num_channels);

    // 48000 bits/second samling rate (DVD quality)
    unsigned int val = 48000;
    int dir;
    snd_pcm_hw_params_set_rate_near(handle, params, &val, &dir);

    snd_pcm_uframes_t frames = period_frame_size;
    snd_pcm_hw_params_set_period_size_near(handle, params, &frames, &dir);

    // Write the parmeters to the driver
    rc = snd_pcm_hw_params(handle, params);
    if(rc < 0) {
        fprintf(stderr, "unable to set hw parameters, reason: %s\n", snd_strerror(rc));
        snd_pcm_close(handle);
        return rc;
    }

    // Use a buffer large enough to hold one period
    snd_pcm_hw_params_get_period_size(params, &frames, &dir);
    int buffer_size = frames * 2 * num_channels; // 2 bytes/sample, @num_channels channels
    void *buffer = malloc(buffer_size);
    if(!buffer) {
        fprintf(stderr, "failed to allocate buffer for audio\n");
        snd_pcm_close(handle);
        return -1;
    }

    device->handle = (void*)handle;
    device->buffer = buffer;
    device->buffer_size = buffer_size;
    device->frames = frames;
    return 0;
}

void sound_device_close(SoundDevice *device) {
    /* TODO: Is this also needed in @sound_device_get_by_name on failure? */
    // TODO: This has been commented out since it causes the thread to block forever. Why?
    //snd_pcm_drain((snd_pcm_t*)device->handle);
    snd_pcm_close((snd_pcm_t*)device->handle);
    free(device->buffer);
}

int sound_device_read_next_chunk(SoundDevice *device, void **buffer) {
    int rc = snd_pcm_readi((snd_pcm_t*)device->handle, device->buffer, device->frames);
    if (rc == -EPIPE) {
        /* overrun */
        fprintf(stderr, "overrun occured\n");
        snd_pcm_prepare((snd_pcm_t*)device->handle);
        return rc;
    } else if(rc < 0) {
        fprintf(stderr, "failed to read from sound device, reason: %s\n", snd_strerror(rc));
        return rc;
    } else if (rc != (int)device->frames) {
        fprintf(stderr, "short read, read %d frames\n", rc);
    }
    *buffer = device->buffer;
    return rc;
}

int sound_device_get_buffer_size(SoundDevice *device) {
    return device->buffer_size;
}
