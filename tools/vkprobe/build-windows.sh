#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Cross-compile vkprobe.exe (x86_64 Windows) for running INSIDE a Winlator
# container. That is the only way to measure the Vulkan device VKD3D-Proton
# actually sees: a Termux binary reaches the system driver, not the Turnip
# instance AdrenoTools injected into the Winlator app process.
#
#   ./build-windows.sh          -> build/vkprobe.exe
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/build"
CC_WIN="${CC_WIN:-x86_64-w64-mingw32-gcc}"

command -v "$CC_WIN" >/dev/null 2>&1 || {
  echo "build-windows.sh: $CC_WIN not found (apt install mingw-w64)" >&2
  exit 1
}

# The Vulkan headers must be reachable WITHOUT putting the host's libc headers
# on the include path -- mingw picks up glibc's stdint.h and dies. So stage
# just the Vulkan trees into a private include dir.
inc="$out/include"
mkdir -p "$inc"
src=""
for c in /usr/include /usr/local/include "${VULKAN_SDK:-/nonexistent}/include"; do
  [[ -d "$c/vulkan" ]] && src="$c" && break
done
[[ -n "$src" ]] || { echo "build-windows.sh: no vulkan/ headers found" >&2; exit 1; }

cp -r "$src/vulkan" "$inc/"
[[ -d "$src/vk_video" ]] && cp -r "$src/vk_video" "$inc/"

mkdir -p "$out"
"$CC_WIN" -O2 -Wall -Wextra -std=c11 -I"$inc" \
          -o "$out/vkprobe.exe" "$here/vkprobe.c"

echo "built: $out/vkprobe.exe"
file "$out/vkprobe.exe" 2>/dev/null || true

cat <<'MSG'

Run it inside the container, not in Termux:
  1. copy vkprobe.exe into the Winlator container's drive_c
  2. launch it from Winlator's file explorer / shortcut, redirecting output:
       vkprobe.exe > Z:\tmp\caps-turnip.json
  3. pull that JSON out and run tools/report.sh on it

Check driver_properties.driverName in the result. MESA_TURNIP means you
measured Turnip; QUALCOMM_PROPRIETARY means the container is on the vendor
blob and the custom driver did not take.
MSG
