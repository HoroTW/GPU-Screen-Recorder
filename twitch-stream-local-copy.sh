#!/bin/sh

# Stream on twitch while also saving the video to disk locally

[ "$#" -ne 5 ] && echo "usage: twitch-stream-local-copy.sh <window_id> <fps> <audio_input> <livestream_key> <local_file>" && exit 1
./gpu-screen-recorder -w "$1" -c flv -f "$2" -a "$3" | tee -- "$5" | ffmpeg -i pipe:0 -c:v copy -f flv -- "rtmp://live.twitch.tv/app/$4"
