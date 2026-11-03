#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk=${TINA_SDK_ROOT:-$(cd "$project/../../../../.." && pwd)}
vendor="$project/vendor/100ask-cv"
cc=${V853_CXX:-$sdk/prebuilt/rootfsbuilt/arm/toolchain-sunxi-musl-gcc-830/toolchain/bin/arm-openwrt-linux-g++}
export STAGING_DIR=${STAGING_DIR:-$sdk/out/v853/vision/openwrt/staging_dir/target}
mkdir -p "$project/build-v853/bin"
"$cc" -Os -Wall -std=c++11 -I"$project/include" -I"$vendor/100ASK_Algo/include" -I"$vendor/sdk_release/sdk" \
    "$project/tools/cv_worker.cpp" "$project/tools/yolo_backend.cpp" -L"$vendor/100ASK_Algo" -L"$vendor/sdk_release" \
    -l:libawnn.a -l:libVIPlite.a -l:libVIPuser.a -lpthread -ldl -lm \
    -o "$project/build-v853/bin/webserver_cv"
