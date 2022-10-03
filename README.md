# gpu screen recorder
This is a screen recorder that has minimal impact on system performance by recording a window using the GPU only,
similar to shadowplay on windows. This is the fastest screen recording tool for Linux.

This screen recorder can be used for recording your desktop offline, for live streaming and for nvidia-like instant replay,
where only the last few seconds are saved.

## Note
For NvFBC to work with PRIME, you must set the primary GPU to your dedicated Nvidia graphics card. On Pop OS, you can select the 'NVIDIA Graphics' option in the power menu, or on Arch Linux you can use Optimus Manager.\
If you are using a variable refresh rate monitor, then choose to record "screen-direct". This will allow variable refresh rate to work when recording fullscreen applications. Note that some applications such as mpv will not work in fullscreen mode. A fix is being developed for this.

# Performance
When recording Legend of Zelda Breath of the Wild at 4k, fps drops from 30 to 7 when using OBS Studio + nvenc, however when using this screen recorder the fps remains at 30.\
When recording GTA V at 4k on highest settings, fps drops from 60 to 23 when using obs-nvfbc + nvenc, however when using this screen recorder the fps only drops to 55. The quality is also much better when using gpu-screen-recorder.\
It is recommended to save the video to a SSD because of the large file size, which a slow HDD might not be fast enough to handle.\
Using NvFBC (recording the monitor/screen) is not faster than not using NvFBC (recording a single window) with gpu screen recorder, in fact it might be a tiny bit slower.

# Installation
If you are running an Arch Linux based distro, then you can find gpu screen recorder on aur under the name gpu-screen-recorder-git (`yay -S gpu-screen-recorder-git`).\
If you are running an Ubuntu based distro then run `install_ubuntu.sh` as root: `sudo ./install_ubuntu.sh`.\
If you are running another distro then you can run `install.sh` as root: `sudo ./install.sh`, but you need to manually install the dependencies, as described below.\
You can also install gpu screen recorder ([the gtk gui version](https://git.dec05eba.com/gpu-screen-recorder-gtk/)) from flathub: https://flathub.org/apps/details/com.dec05eba.gpu_screen_recorder.

# Dependencies
`libgl (libglvnd), ffmpeg, libx11, libxcomposite, libpulse`. You need to additionally have `cuda` installed when you run `gpu-screen-recorder`.\
Recording monitors requires a gpu with NvFBC support (note: this is not required when recording a single window!). Normally only tesla and quadro gpus support this, but by using [nvidia-patch](https://github.com/keylase/nvidia-patch) or [nvlax](https://github.com/illnyang/nvlax) you can do this on all gpus that support nvenc as well (gpus as old as the nvidia 600 series), provided you are not using outdated gpu drivers.

# How to use
Run `scripts/interactive.sh` or run gpu-screen-recorder directly, for example: `gpu-screen-recorder -w $(xdotool selectwindow) -c mp4 -f 60 -a "$(pactl get-default-sink).monitor" -o test_video.mp4`\
Then stop the screen recorder with Ctrl+C, which will also save the recording.\
Send signal SIGUSR1 (`killall -SIGUSR1 gpu-screen-recorder`) to gpu-screen-recorder when in replay mode to save the replay. The paths to the saved files is output to stdout after the recording is saved.\
You can find the default output audio device (headset, speakers (in other words, desktop audio)) with the command `pactl get-default-sink`. Add `monitor` to the end of that to use that as an audio input in gpu-screen-recorder.\
You can find the default input audio device (microphone) with the command `pactl get-default-source`. This input should not have `monitor` added to the end when used in gpu-screen-recorder.\
Example of recording both desktop audio and microphone: `gpu-screen-recorder -w $(xdotool selectwindow) -c mp4 -f 60 -a "$(pactl get-default-sink).monitor" -a "$(pactl get-default-source)" -o test_video.mp4`.\
Note that if you use multiple audio inputs then they are each recorded into separate audio tracks in the video file. There is currently no option to merge audio tracks, but it's a planned feature.

There is also a gui for the gpu-screen-recorder called [gpu-screen-recorder-gtk](https://git.dec05eba.com/gpu-screen-recorder-gtk/).

# Demo
[![Click here to watch a demo video on youtube](https://img.youtube.com/vi/n5tm0g01n6A/0.jpg)](https://www.youtube.com/watch?v=n5tm0g01n6A)

# FAQ
## How is this different from using OBS with nvenc?
OBS only uses the gpu for video encoding, but the window image that is encoded is copied from the GPU to the CPU and then back to the GPU (video encoding unit). These operations are very slow and causes all of the fps drops when using OBS. OBS only uses the GPU efficiently on Windows 10 and Nvidia.\
This gpu-screen-recorder keeps the window image on the GPU and sends it directly to the video encoding unit on the GPU by using CUDA. This means that CPU usage remains at around 0% when using this screen recorder.
## How is this different from using OBS NvFBC plugin?
The plugin does everything on the GPU and gives the texture to OBS, but OBS does not know how to use the texture directly on the GPU so it copies the texture to the CPU and then back to the GPU (video encoding unit). These operations are very slow and causes a lot of fps drops unless you have a fast CPU. This is especially noticable when recording at higher resolutions than 1080p.
## How is this different from using FFMPEG with x11grab and nvenc?
FFMPEG only uses the GPU with CUDA when doing transcoding from an input video to an output video, and not when recording the screen when using x11grab. So FFMPEG has the same fps drop issues that OBS has.

# TODO
* Support AMD and Intel, using VAAPI.
libraries at compile-time.
* Clean up the code!
* Dynamically change bitrate/resolution to match desired fps. This would be helpful when streaming for example, where the encode output speed also depends on upload speed to the streaming service.
* Show cursor when recording. Currently the cursor is not visible when recording a window and it's disabled when recording screen-direct to allow direct nvfbc capture for fullscreen windows, which allows for better performance and variable refresh rate monitors to work.
* Implement opengl injection to capture texture. This fixes composition issues and (VRR) without having to use NvFBC direct capture.
* Always use direct capture with NvFBC once the capture issue in mpv fullscreen has been resolved (maybe detect if direct capture fails in nvfbc and switch to non-direct recording. NvFBC says if direct capture fails).
