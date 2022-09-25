#!/bin/sh -e

dependencies="glew libavcodec libavformat libavutil x11 xcomposite glfw3 libpulse libswresample"
includes="$(pkg-config --cflags $dependencies) -Iinclude"
libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
g++ -c src/sound.cpp -O2 $includes
g++ -c src/main.cpp -O2 $includes
g++ -o gpu-screen-recorder -O2 sound.o main.o -s $libs
