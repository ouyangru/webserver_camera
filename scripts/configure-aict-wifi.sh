#!/usr/bin/env bash
# Apply only the three XR829 settings verified on the user's AICT-V853 board.
set -euo pipefail
project=$(cd "$(dirname "$0")/.." && pwd)
sdk=${TINA_SDK_ROOT:-$(cd "$project/../../../../.." && pwd)}
config="$sdk/openwrt/openwrt/.config"
editor="$sdk/kernel/linux-4.9/scripts/config"
[[ -f "$config" && -f "$editor" ]] || {
    echo 'Set TINA_SDK_ROOT to the existing Tina SDK with a selected OpenWrt configuration.' >&2
    exit 1
}
if [[ ! -e "$config.before-aict-wifi" ]]; then cp -p "$config" "$config.before-aict-wifi"; fi
bash "$editor" --file "$config" --disable XR829_USE_40M_SDD \
    --disable PACKAGE_kmod-net-xr829-40M --enable PACKAGE_kmod-net-xr829
echo 'AICT XR829 default SDD selected. Rebuild OpenWrt rootfs and pack the image.'
