#!/bin/sh -e

dependencies="libavcodec libavformat libavutil x11 xcomposite xrandr libpulse libswresample"
includes="$(pkg-config --cflags $dependencies)"
libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
gcc -c src/capture/capture.c -O2 -g0 -DNDEBUG $includes
gcc -c src/capture/nvfbc.c -O2 -g0 -DNDEBUG $includes
gcc -c src/capture/xcomposite.c -O2 -g0 -DNDEBUG $includes
gcc -c src/gl.c -O2 -g0 -DNDEBUG $includes
gcc -c src/cuda.c -O2 -g0 -DNDEBUG $includes
gcc -c src/window_texture.c -O2 -g0 -DNDEBUG $includes
gcc -c src/time.c -O2 -g0 -DNDEBUG $includes
g++ -c src/sound.cpp -O2 -g0 -DNDEBUG $includes
g++ -c src/main.cpp -O2 -g0 -DNDEBUG $includes
g++ -o gpu-screen-recorder -O2 capture.o nvfbc.o gl.o cuda.o window_texture.o time.o xcomposite.o sound.o main.o -s $libs
echo "Successfully built gpu-screen-recorder"