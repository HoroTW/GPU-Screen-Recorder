#!/bin/sh

[ "$#" -ne 2 ] && echo "usage: twitch-stream.sh <window_id> <livestream_key>" && exit 1
sibs build --release && ./sibs-build/linux_x86_64/release/hardware-screen-recorder "$1" "dummy.h264" | ffmpeg -i pipe:0 -c:v copy -f flv "rtmp://live.twitch.tv/app/$2"
