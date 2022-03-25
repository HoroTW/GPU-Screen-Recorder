#!/bin/sh -e

selected_audio_input="$(pactl get-default-sink).monitor"

echo "Select a window to record"
window_id=$(xdotool selectwindow)

echo -n "Enter video fps: "
read fps

echo -n "Enter output file name: "
read output_file_name

output_dir=$(dirname "$output_file_name")
mkdir -p "$output_dir"

./gpu-screen-recorder -w "$window_id" -c mp4 -f "$fps" -a "$selected_audio_input" -o "$output_file_name"
