#!/bin/sh

[ "$#" -ne 3 ] && echo "usage: twitch-stream.sh <window_id> <fps> <livestream_key>" && exit 1
sibs build --release && ./sibs-build/linux_x86_64/release/hardware-screen-recorder "$1" h264 "$2" | ffmpeg -i pipe:0 -c:v copy -f flv "rtmp://live.twitch.tv/app/$3"
