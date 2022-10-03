#!/bin/sh

script_dir=$(dirname "$0")
cd "$script_dir"

[ $(id -u) -ne 0 ] && echo "You need root privileges to run the install script" && exit 1

./build.sh
install -Dm755 "gpu-screen-recorder" "/usr/local/bin/gpu-screen-recorder"
install -Dm755 "gpu-screen-recorder" "/usr/bin/gpu-screen-recorder"
echo "Successfully installed gpu-screen-recorder"
