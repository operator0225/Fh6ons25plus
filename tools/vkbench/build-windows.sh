#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Cross-compile vkbench.exe for running inside a Winlator container.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/build"
CC_WIN="${CC_WIN:-x86_64-w64-mingw32-gcc}"

command -v "$CC_WIN" >/dev/null 2>&1 || {
  echo "build-windows.sh: $CC_WIN not found (apt install mingw-w64)" >&2; exit 1; }

# Stage only the Vulkan headers: putting the host /usr/include on mingw's
# path pulls in glibc's stdint.h and fails the build.
inc="$out/include"; mkdir -p "$inc"
src=""
for c in /usr/include /usr/local/include "${VULKAN_SDK:-/nonexistent}/include"; do
  [[ -d "$c/vulkan" ]] && src="$c" && break
done
[[ -n "$src" ]] || { echo "build-windows.sh: no vulkan/ headers found" >&2; exit 1; }
cp -r "$src/vulkan" "$inc/"
[[ -d "$src/vk_video" ]] && cp -r "$src/vk_video" "$inc/"

"$CC_WIN" -O2 -Wall -Wextra -std=c11 -I"$inc" -I"$here" \
          -o "$out/vkbench.exe" "$here/vkbench.c"
echo "built: $out/vkbench.exe"
file "$out/vkbench.exe" 2>/dev/null || true
