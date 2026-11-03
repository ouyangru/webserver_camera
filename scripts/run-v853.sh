#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
output="$project/build-v853"
export LD_LIBRARY_PATH="$output/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export WEBSERVER_CAMERA_RESOURCES="$output/resources"
exec "$output/bin/webserver_camera" "${1:-10000}" cedar
