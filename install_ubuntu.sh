#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

set -e
apt-get -y install build-essential\
	libswresample-dev libavformat-dev libavcodec-dev libavutil-dev\
	libgl-dev libx11-dev libxcomposite-dev\
	libpulse-dev

./install.sh
