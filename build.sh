#!/bin/sh -e

dependencies="glew libavcodec libavformat libavutil x11 xcomposite glfw3 libpulse-simple libswresample"
includes="$(pkg-config --cflags $dependencies) -I/opt/cuda/targets/x86_64-linux/include"
libs="$(pkg-config --libs $dependencies) /usr/lib/libcuda.so -ldl -pthread -lm"
g++ -c src/sound.cpp -O2 $includes -DPULSEAUDIO=1
g++ -c src/main.cpp -O2 $includes -DPULSEAUDIO=1
g++ -o gpu-screen-recorder -O2 sound.o main.o -s $libs
