#!/bin/sh -e

print_selected_window_id() {
    xwininfo | grep 'Window id:' | cut -d' ' -f4
}

echo "Select a window to record"
window_id=$(print_selected_window_id)

echo -n "Enter video fps: "
read fps

echo "Select audio input:"
selected_audio_input=""
select audio_input in $(pactl list | sed -rn 's/Monitor Source: (.*)/\1/p'); do
    if [ "$audio_input" == "" ]; then
        echo "Invalid option $REPLY"
    else
        selected_audio_input="$audio_input"
        break
    fi
done

echo -n "Enter output file name: "
read output_file_name

output_dir=$(dirname "$output_file_name")
mkdir -p "$output_dir"

sibs build --release && ./sibs-build/linux_x86_64/release/gpu-screen-recorder -w "$window_id" -c mp4 -f "$fps" -a "$selected_audio_input" > "$output_file_name"
