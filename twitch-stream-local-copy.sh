#!/bin/sh

# Stream on twitch while also saving the video to disk locally

[ "$#" -ne 4 ] && echo "usage: twitch-stream-local-copy.sh <window_id> <fps> <livestream_key> <local_file>" && exit 1
active_sink="$(pactl get-default-sink).monitor"
./gpu-screen-recorder -w "$1" -c flv -f "$2" -a "$active_sink" | tee -- "$4" | ffmpeg -i pipe:0 -c:v copy -f flv -- "rtmp://live.twitch.tv/app/$3"
