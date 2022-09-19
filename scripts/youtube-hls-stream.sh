#!/bin/sh

[ "$#" -ne 3 ] && echo "usage: youtube-hls-stream.sh <window_id> <fps> <livestream_key>" && exit 1
mkdir "youtube_stream"
cd "youtube_stream"
active_sink="$(pactl get-default-sink).monitor"
gpu-screen-recorder -w "$1" -c mpegts -f "$2" -a "$active_sink" | ffmpeg -i pipe:0 -c copy -f hls \
    -hls_time 2 -hls_flags independent_segments -hls_flags delete_segments -hls_segment_type mpegts -hls_segment_filename stream%02d.ts -master_pl_name stream.m3u8 out1 &
echo "Waiting until stream segments are created..."
sleep 10
ffmpeg -i stream.m3u8 -c copy -- "https://a.upload.youtube.com/http_upload_hls?cid=$3&copy=0&file=stream.m3u8"
