# gpu screen recorder
This is a screen recorder that has minimal impact on system performance by recording a window using the GPU only,
similar to shadowplay on windows.

The output is an h264 encoded video with aac audio.

This project is still early in development.

# Performance
When recording a 4k game, fps drops from 30 to 7 when using OBS Studio, however when using this screen recorder
the fps remains at 30.

# Example
`gpu-screen-recorder -w 0x1c00001 -c mp4 -f 60 -a bluez_sink.00_18_09_8A_07_93.a2dp_sink.monitor > test_video.mp4`

# Demo
[![Click here to watch a demo video on youtube](https://img.youtube.com/vi/n5tm0g01n6A/0.jpg)](https://www.youtube.com/watch?v=n5tm0g01n6A)

# Requirements
X11, Nvidia (cuda), alsa or pulseaudio

# TODO
* Scale video when the window is rescaled.
* Support AMD and Intel, using VAAPI. cuda and vaapi should be loaded at runtime using dlopen instead of linking to those
libraries at compile-time.
* Clean up the code!
