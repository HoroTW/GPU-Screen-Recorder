#!/usr/bin/env bash

while true; do
    if pgrep -f -x "^gpu-screen-recorder\s.*$" >/dev/null; then
        echo "gpu-screen-recorder is already running"
        break
    else
        echo "gpu-screen-recorder is not running. Trying to start..."
        gpu-screen-recorder -e true -w DP-0 -c mp4 -q very_high -k auto -ac opus -f 60 -r 300 -o /home/horo/Videos -a alsa_output.pci-0000_05_04.0.analog-stereo.monitor -a easyeffects_source # find the command by using the help, or use the `qt version` and check with `htop` what it uses PLUS ADD THE `-e true` option!
        sleep 1
    fi
done
