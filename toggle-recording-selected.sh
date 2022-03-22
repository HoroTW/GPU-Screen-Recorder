#!/bin/sh -e

killall -INT gpu-screen-recorder && notify-send -u low 'GPU Screen Recorder' 'Stopped recording' && exit 0;
window=$(xdotool selectwindow)
active_sink="$(pactl get-default-sink).monitor"
mkdir -p "$HOME/Videos"
video="$HOME/Videos/$(date +"Video_%Y-%m-%d_%H-%M-%S.mp4")"
notify-send -u low 'GPU Screen Recorder' "Started recording video to $video"
gpu-screen-recorder -w "$window" -c mp4 -f 60 -a "$active_sink" -o "$video"
