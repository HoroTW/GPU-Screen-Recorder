#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

dpkg -l cuda > /dev/null 2>&1
cuda_missing="$?"

set -e
apt-get -y install build-essential cuda\
	libswresample-dev libavformat-dev libavcodec-dev libavutil-dev\
	libx11-dev libxcomposite-dev\
	libglew-dev libglfw3-dev\
	libpulse-dev

dependencies="glew libavcodec libavformat libavutil x11 xcomposite glfw3 libpulse libswresample"
includes="$(pkg-config --cflags $dependencies) -Iinclude"
libs="$(pkg-config --libs $dependencies) -ldl -pthread -lm"
g++ -c src/sound.cpp -O2 $includes
g++ -c src/main.cpp -O2 $includes
g++ -o gpu-screen-recorder -O2 sound.o main.o -s $libs
install -Dm755 "gpu-screen-recorder" "/usr/local/bin/gpu-screen-recorder"

echo "Successfully installed gpu-screen-recorder"
[ "$cuda_missing" -eq 1 ] && echo "You need to reboot your computer before using gpu-screen-recorder because cuda was installed"
