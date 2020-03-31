# Hardware screen recorder
This is a screen recorder that has minimal impact on system performance by recording a window using the GPU only,
similar to shadowplay on windows.

This project is still early in development.

# Performance
When recording a 4k game, fps drops from 30 to 7 when using OBS Studio, however when using this screen recorder
the fps remains at 30.

# Requirements
X11, Nvidia (cuda)

# TODO
* Scale video when the window is rescaled.
* Use the sound source in src/sound.cpp to record audio and mux it with ffmpeg to the final video.
* Support AMD and Intel, using VAAPI. cuda and vaapi should be loaded at runtime using dlopen instead of linking to those
libraries at compile-time.
* Clean up the code!
