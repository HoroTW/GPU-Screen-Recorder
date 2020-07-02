#!/bin/sh

[ "$#" -ne 4 ] && echo "usage: twitch-stream.sh <window_id> <fps> <audio_input> <livestream_key>" && exit 1
#ismv
sibs build --release && ./sibs-build/linux_x86_64/release/gpu-screen-recorder -w "$1" -c flv -f "$2" -a "$3" | ffmpeg -i pipe:0 -c:v copy -f flv -max_muxing_queue_size 4096 -- "rtmp://live.twitch.tv/app/$4"
