#!/usr/bin/env bash
# Standalone V853 application build; firmware packages continue to use Makefile.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk_dir=${TINA_SDK_ROOT:-}
if [[ -z "$sdk_dir" ]]; then
    candidate_dir=$project_dir
    while [[ "$candidate_dir" != / ]]; do
        if [[ -d "$candidate_dir/prebuilt/rootfsbuilt/arm/toolchain-sunxi-musl-gcc-830/toolchain" ]]; then
            sdk_dir=$candidate_dir
            break
        fi
        candidate_dir=$(dirname -- "$candidate_dir")
    done
fi
if [[ -z "$sdk_dir" ]]; then
    echo 'Cannot find Tina SDK. Set TINA_SDK_ROOT to the SDK root directory.' >&2
    exit 1
fi
toolchain_dir="$sdk_dir/prebuilt/rootfsbuilt/arm/toolchain-sunxi-musl-gcc-830/toolchain"
compiler_prefix="$toolchain_dir/bin/arm-openwrt-linux-"
for tool in gcc g++ ar strip; do
    if [[ ! -x "${compiler_prefix}${tool}" ]]; then
        echo "Missing tool: ${compiler_prefix}${tool}" >&2
        exit 1
    fi
done
export PATH="$toolchain_dir/bin:$PATH"
export STAGING_DIR="$sdk_dir/out/v853/vision/openwrt/staging_dir/target"
output_dir="$project_dir/build-v853"
cedar_root="$project_dir/vendor/cedarc-v853"
bash "$project_dir/scripts/build-cedar-runtime.sh"
mkdir -p "$output_dir/obj" "$output_dir/bin" "$output_dir/resources"
target_flags='-Os -pipe -mfloat-abi=hard'
echo "Compiler: $("${compiler_prefix}g++" -dumpmachine)"
# Rebuild from source each time so header and flag changes cannot reuse stale objects.
make -C "$project_dir" -f app.mk -B -j"${JOBS:-2}" \
    CC="${compiler_prefix}gcc" CXX="${compiler_prefix}g++" \
    AR="${compiler_prefix}ar" STRIP="${compiler_prefix}strip" \
    CPPFLAGS="-I$STAGING_DIR/usr/include -I$toolchain_dir/usr/include -I$toolchain_dir/include" \
    CFLAGS="$target_flags" CXXFLAGS="$target_flags" \
    LDFLAGS="$target_flags -L$STAGING_DIR/usr/lib -L$toolchain_dir/usr/lib -L$toolchain_dir/lib" \
    ENABLE_GSTREAMER=0 ENABLE_JPEG=1 ENABLE_CEDAR=1 \
    CEDARC_ROOT="$cedar_root" CEDARC_LIBDIR="$output_dir/lib" TARGET_NAME=webserver_camera \
    OBJ_DIR="$output_dir/obj" BIN_DIR="$output_dir/bin" all
cp -a "$project_dir/resources/." "$output_dir/resources/"
bash "$project_dir/scripts/build-cv.sh"
"${compiler_prefix}strip" "$output_dir/bin/webserver_camera" "$output_dir/bin/h264_to_flv"
file "$output_dir/bin/webserver_camera"
echo "Build complete: $output_dir"
echo 'Upload both bin/webserver_camera and resources/ together via ADB.'
