# gpu screen recorder
This is a screen recorder that has minimal impact on system performance by recording a window using the GPU only,
similar to shadowplay on windows. This is the fastest screen recording tool for Linux.

This screen recorder can be used for recording your desktop offline, for live streaming and for nvidia-like instant replay,
where only the last few seconds are saved.

The output is an h264 encoded video with aac audio.

# Performance
When recording a 4k game, fps drops from 30 to 7 when using OBS Studio, however when using this screen recorder
the fps remains at 30.

# Installation
gpu screen recorder can be built using [sibs](https://git.dec05eba.com/sibs) or if you are running Arch Linux, then you can find it on aur under the name gpu-screen-recorder-git (`yay -S gpu-screen-recorder-git`).\
Recording displays requires a gpu with NvFBC support. Normally only tesla and quadro gpus support this, but by using https://github.com/keylase/nvidia-patch you can do this on all gpus that support nvenc as well (gpus as old as the nvidia 600 series), provided you are not using outdated gpu drivers.

# How to use
Run `interactive.sh` or run gpu-screen-recorder directly, for example: `gpu-screen-recorder -w 0x1c00001 -c mp4 -f 60 -a bluez_sink.00_18_09_8A_07_93.a2dp_sink.monitor > test_video.mp4`\
Then stop the screen recorder with Ctrl+C.\
There is also a gui for the gpu-screen-recorder, called [gpu-screen-recorder-gtk](https://git.dec05eba.com/gpu-screen-recorder-gtk/).

# Demo
[![Click here to watch a demo video on youtube](https://img.youtube.com/vi/n5tm0g01n6A/0.jpg)](https://www.youtube.com/watch?v=n5tm0g01n6A)

# Requirements
X11, Nvidia (cuda), alsa or pulseaudio

# FAQ
## How is this different from using OBS with nvenc?
OBS only uses the gpu for video encoding, but the window image that is encoded is sent from the GPU to the CPU and then back to the GPU. These operations are very slow and causes all of the fps drops when using OBS. OBS only uses the GPU efficiently on Windows 10 and Nvidia.\
This gpu-screen-recorder keeps the window image on the GPU and sends it directly to the video encoding unit on the GPU by using CUDA. This means that CPU usage remains at around 0% when using this screen recorder.
## How is this different from using FFMPEG with x11grab and nvenc?
FFMPEG only uses the GPU with CUDA when doing transcoding from an input video to an output video, and not when recording the screen when using x11grab. So FFMPEG has the same fps drop issues that OBS has.

# TODO
* Scale video when the window is rescaled.
* Support AMD and Intel, using VAAPI. cuda and vaapi should be loaded at runtime using dlopen instead of linking to those
libraries at compile-time.
* Clean up the code!
* Fix segfault in debug mode (happens because audio codec becomes NULL?)
* Dynamically change bitrate to match desired fps. This would be helpful when streaming for example, where the encode output speed also depends on upload speed to the stream service.
