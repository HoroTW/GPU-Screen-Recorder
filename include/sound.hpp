#ifndef GPU_SCREEN_RECORDER_H
#define GPU_SCREEN_RECORDER_H

typedef struct {
    void *handle;
    void *buffer;
    int buffer_size;
    unsigned int frames;
} SoundDevice;

/*
    Get a sound device by name, returning the device into the @device parameter.
    The device should be closed with @sound_device_close after it has been used
    to clean up internal resources.
    Returns 0 on success, or a negative value on failure.
*/
int sound_device_get_by_name(SoundDevice *device, const char *name = "default", unsigned int num_channels = 2, unsigned int period_frame_size = 32);

void sound_device_close(SoundDevice *device);

/*
    Returns the next chunk of audio into @buffer.
    Returns the number of frames read, or a negative value on failure.
*/
int sound_device_read_next_chunk(SoundDevice *device, void **buffer);

int sound_device_get_buffer_size(SoundDevice *device);

#endif /* GPU_SCREEN_RECORDER_H */
