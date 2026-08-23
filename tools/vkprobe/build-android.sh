#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Cross-compile vkprobe for arm64 Android using the NDK.
#
#   ANDROID_NDK_HOME=/path/to/ndk ./build-android.sh
#
# Output: build/arm64-v8a/vkprobe
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
out="$here/build/arm64-v8a"
api="${ANDROID_API:-30}"

ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${NDK_HOME:-}}}"
if [[ -z "$ndk" ]]; then
  for c in "$HOME/Android/Sdk/ndk"/* /opt/android-ndk* /usr/lib/android-ndk; do
    [[ -d "$c" ]] && ndk="$c" && break
  done
fi

if [[ -z "$ndk" || ! -d "$ndk" ]]; then
  cat >&2 <<'EOF'
build-android.sh: no Android NDK found.

Set ANDROID_NDK_HOME to an NDK r26 or newer, or build natively on-device
under Termux instead:

    pkg install clang vulkan-headers
    cc -O2 -o vkprobe vkprobe.c -ldl

Termux is usually the faster route: it needs no host SDK, and the binary
runs in the same place you are going to measure.
EOF
  exit 1
fi

host_tag="linux-x86_64"
[[ "$(uname -s)" == "Darwin" ]] && host_tag="darwin-x86_64"
tc="$ndk/toolchains/llvm/prebuilt/$host_tag"
cc="$tc/bin/aarch64-linux-android${api}-clang"

if [[ ! -x "$cc" ]]; then
  echo "build-android.sh: compiler not found: $cc" >&2
  echo "  (check ANDROID_API=$api is available in this NDK)" >&2
  exit 1
fi

mkdir -p "$out"
# Android's libvulkan.so is dlopen'd at runtime, not linked, so no -lvulkan.
"$cc" -O2 -Wall -Wextra -std=c11 -fPIE -pie \
      -o "$out/vkprobe" "$here/vkprobe.c"

echo "built: $out/vkprobe"
file "$out/vkprobe" 2>/dev/null || true

cat <<EOF

Push and run:
    adb push $out/vkprobe /data/local/tmp/
    adb shell chmod 755 /data/local/tmp/vkprobe
    adb shell /data/local/tmp/vkprobe > adreno830-system-driver.json

That measures the SYSTEM driver (the Qualcomm blob). To measure Turnip you
must point at the Turnip ICD explicitly:
    adb shell /data/local/tmp/vkprobe --icd /path/to/freedreno_icd.aarch64.json

Check driver_properties.driverName in the output to confirm which one
actually answered. If it says the wrong driver, the numbers are the wrong
driver's numbers.
EOF
