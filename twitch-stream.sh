#!/bin/sh

[ "$#" -ne 4 ] && echo "usage: twitch-stream.sh <window_id> <fps> <audio_input> <livestream_key>" && exit 1
./gpu-screen-recorder -w "$1" -c flv -f "$2" -a "$3" | ffmpeg -i pipe:0 -c:v copy -f flv -- "rtmp://live.twitch.tv/app/$4"
