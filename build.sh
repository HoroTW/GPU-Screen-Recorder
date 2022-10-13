#!/bin/sh -e

dependencies="libavcodec libavformat libavutil x11 xcomposite xrandr libpulse libswresample"
includes="$(pkg-config --cflags $dependencies)"
libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
gcc -c src/capture/capture.c -O2 -g0 -DNDEBUG $includes
gcc -c src/capture/nvfbc.c -O2 -g0 -DNDEBUG $includes
g++ -c src/sound.cpp -O2 -g0 -DNDEBUG $includes
g++ -c src/main.cpp -O2 -g0 -DNDEBUG $includes
g++ -o gpu-screen-recorder -O2 capture.o nvfbc.o sound.o main.o -s $libs
echo "Successfully built gpu-screen-recorder"