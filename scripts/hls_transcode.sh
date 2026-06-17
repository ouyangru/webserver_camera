#!/usr/bin/env bash
set -euo pipefail

# Simple HLS transcode to resources/hls
# Usage: ./scripts/hls_transcode.sh [source] [width] [height] [fps]
# source can be /dev/video0 or http://IP:10000/hls/stream.m3u8

SOURCE="${1:-http://127.0.0.1:10000/live}"
WIDTH="${2:-640}"
HEIGHT="${3:-480}"
FPS="${4:-25}"
OUT_DIR="${OUT_DIR:-./resources/hls}"

mkdir -p "$OUT_DIR"

if [[ "$SOURCE" == http* || "$SOURCE" == rtsp* ]]; then
  INPUT_ARGS=( -i "$SOURCE" )
else
  INPUT_ARGS=( -f v4l2 -input_format nv12 -video_size "${WIDTH}x${HEIGHT}" -framerate "$FPS" -i "$SOURCE" )
fi

ffmpeg \
  "${INPUT_ARGS[@]}" \
  -c:v h264_v4l2m2m -pix_fmt nv12 \
  -g "$FPS" -keyint_min "$FPS" -sc_threshold 0 \
  -f hls -hls_time 1 -hls_list_size 6 \
  -hls_flags delete_segments+append_list+program_date_time \
  "$OUT_DIR/stream.m3u8"
