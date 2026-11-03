#!/usr/bin/env bash
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
sdk=${TINA_SDK_ROOT:-$(cd "$project/../../../../.." && pwd)}
cedar="$project/vendor/cedarc-v853"
cc=${V853_CC:-"$sdk/prebuilt/rootfsbuilt/arm/toolchain-sunxi-musl-gcc-830/toolchain/bin/arm-openwrt-linux-gcc"}
dest="$project/build-v853/lib"
mkdir -p "$dest"
rm -f "$dest"/*.so
includes=(-I"$cedar/include" -I"$cedar/base/include" -I"$cedar/memory/include" -I"$cedar/vencoder/base/include" -I"$cedar/ve/include")
flags=(-Os -fPIC -mfloat-abi=hard -D_GNU_SOURCE -DCONF_KERNEL_VERSION_4_9 -DCONF_KERNEL_CMA -DCONF_USE_IOMMU=1)
export STAGING_DIR=${V853_STAGING_DIR:-"$sdk/out/v853/vision/openwrt/staging_dir/target"}
vendor="$cedar/library/musl"
cp "$vendor/libvenc_codec.so" "$vendor/libVE.so" "$dest/"
build_lib() {
    local name=$1; shift
    "$cc" "${flags[@]}" "${includes[@]}" -shared -Wl,-soname,"$name.so" \
        -o "$dest/$name.so" "$@" -L"$dest" -Wl,-rpath-link,"$dest" -ldl -lpthread -lm -lrt
}
base_sources=("$cedar/base/"*.c)
build_lib libcdc_base "${base_sources[@]}"
build_lib libMemAdapter "$cedar/memory/memoryAdapter.c" "$cedar/memory/ionMemory/ionAlloc.c" -lcdc_base -lVE
build_lib libvenc_base "$cedar/vencoder/base/FrameBufferManager.c" "$cedar/vencoder/base/BitstreamManager.c" "$cedar/vencoder/base/EncAdapter.c" -lMemAdapter -lcdc_base -lVE
build_lib libvencoder "$cedar/vencoder/vencoder.c" -lvenc_base -lMemAdapter -lcdc_base -lVE -lvenc_codec
echo "Cedar runtime built in $dest"
