#!/bin/sh

[ "$#" -ne 5 ] && echo "usage: replay.sh <window_id> <fps> <audio_input> <replay_time_sec> <output_file>" && exit 1
sibs build --release && ./sibs-build/linux_x86_64/release/gpu-screen-recorder -w "$1" -c mp4 -f "$2" -a "$3" -r "$4" -o "$5"
