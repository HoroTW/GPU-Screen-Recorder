#!/bin/sh -e

window=$(xdotool selectwindow)
active_sink="$(pactl get-default-sink).monitor"
mkdir -p "$HOME/Videos"
video="$HOME/Videos/$(date +"Video_%Y-%m-%d_%H-%M-%S.mp4")"
gpu-screen-recorder -w "$window" -c mp4 -f 60 -a "${active_sink}.monitor" -o "$video"
notify-send "GPU Screen Recorder" "Saved video to $video"
