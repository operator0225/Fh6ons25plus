#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# star-bionic diagnose -- Stage 1 environment capture.
#
# Answers the question every later stage depends on: WHICH renderer, driver,
# and translation stack is actually running on this device right now? Nothing
# here infers anything from the device model. Everything is read from the
# running system, and anything that cannot be read is recorded as
# "unavailable" rather than guessed.
#
#   ./diagnose.sh              # run on-device (Termux / adb shell)
#   ./diagnose.sh --adb        # run from a host with adb, targets the device
#   ./diagnose.sh --out DIR    # output directory (default: ./diag-<timestamp>)
#
# Produces DIR/ plus DIR.tar.gz, with a SUMMARY.txt at the top.
set -uo pipefail

MODE=local
OUT=""
ADB_SERIAL=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --adb)    MODE=adb; shift ;;
    --serial) ADB_SERIAL="$2"; shift 2 ;;
    --out)    OUT="$2"; shift 2 ;;
    -h|--help)
      sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "diagnose.sh: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

OUT="${OUT:-diag-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

if [[ "$MODE" == adb ]]; then
  command -v adb >/dev/null 2>&1 || { echo "diagnose.sh: adb not found" >&2; exit 1; }
  ADB=(adb)
  [[ -n "$ADB_SERIAL" ]] && ADB=(adb -s "$ADB_SERIAL")
  sh_exec() { "${ADB[@]}" shell "$@" 2>&1; }
else
  sh_exec() { sh -c "$*" 2>&1; }
fi

# run <name> <description> <command...>
# Never fails the script. Records the exit status so "empty" and "errored"
# are distinguishable later.
run() {
  local name="$1" desc="$2"; shift 2
  local f="$OUT/$name.txt"
  {
    echo "### $desc"
    # Prefix EVERY line: these commands are multi-line scripts, and an
    # unprefixed body would later be grepped as if it were captured output.
    printf '%s\n' "$*" | sed 's/^/### cmd| /'
    echo "### ---"
  } > "$f"
  sh_exec "$@" >> "$f" 2>&1
  local rc=$?
  echo "### exit: $rc" >> "$f"
  printf '%-34s %s\n' "$name" "$([[ $rc -eq 0 ]] && echo ok || echo "rc=$rc")"
}

echo "star-bionic diagnose -> $OUT  (mode: $MODE)"
echo "---------------------------------------------"

# ------------------------------------------------------------------
# Device identity / SoC
# ------------------------------------------------------------------
run device-props "Android build + SoC identity" \
  'getprop | grep -Ei "ro\.product\.(model|device|name|board)|ro\.board\.platform|ro\.soc\.|ro\.hardware|ro\.build\.version\.(release|sdk)|ro\.build\.fingerprint|dalvik\.vm\.isa"'

run kernel "Kernel version" 'uname -a; echo; cat /proc/version'

# ------------------------------------------------------------------
# GPU -- Adreno exposes itself through the KGSL driver in sysfs.
# gpu_model here is the authoritative on-device answer for "which Adreno".
# ------------------------------------------------------------------
run gpu-kgsl "Adreno/KGSL GPU state" '
for d in /sys/class/kgsl/kgsl-3d0 /sys/devices/platform/*.gpu; do
  [ -d "$d" ] || continue
  echo "== $d"
  for f in gpu_model gpu_busy_percentage gpuclk max_gpuclk min_clock_mhz \
           devfreq/cur_freq devfreq/max_freq devfreq/min_freq \
           devfreq/available_frequencies devfreq/governor \
           temp throttling gpu_available_frequencies; do
    [ -r "$d/$f" ] && printf "%-34s %s\n" "$f" "$(cat "$d/$f" 2>/dev/null)"
  done
done'

run gpu-egl "GLES/EGL driver strings (vendor blob identity)" \
  'getprop | grep -Ei "ro\.hardware\.(vulkan|egl)|ro\.gfx|debug\.gr"; ls -l /vendor/lib64/egl/ /vendor/lib64/hw/vulkan.* 2>/dev/null'

# ------------------------------------------------------------------
# Vulkan -- which ICDs exist on disk, and which one the loader picks.
# ------------------------------------------------------------------
run vulkan-icd "Vulkan ICD/driver files present" '
echo "== vendor vulkan HAL"; ls -l /vendor/lib64/hw/vulkan.*.so 2>/dev/null
echo "== loader"; ls -l /system/lib64/libvulkan.so /vendor/lib64/libvulkan.so 2>/dev/null
echo "== Mesa-style ICD json / driver .so"
find /data /sdcard /storage/emulated/0 -maxdepth 6 \
     \( -name "*icd*.json" -o -name "libvulkan_freedreno.so" -o -name "*freedreno*" \) \
     2>/dev/null | head -50'

run vulkan-tools "vulkaninfo, if present" \
  'command -v vulkaninfo >/dev/null 2>&1 && vulkaninfo --summary || echo "vulkaninfo not installed (Termux: pkg install vulkan-tools)"'

# ------------------------------------------------------------------
# Winlator-family packages and their containers
# ------------------------------------------------------------------
run packages "Installed Winlator-family packages" \
  'pm list packages 2>/dev/null | grep -Ei "winlator|gamehub|gamenative|winhub|mobox|termux|exagear" || echo "pm unavailable or no matches"'

run containers "Winlator container / imagefs layout" '
for base in /sdcard/Winlator /storage/emulated/0/Winlator /storage/emulated/0/GameHub \
            /storage/emulated/0/GameNative /data/data/com.winlator/files; do
  [ -d "$base" ] || continue
  echo "== $base"
  ls -la "$base" 2>/dev/null | head -40
done'

# The DLL set inside a container tells you which translation layers are live.
# d3d12.dll + d3d12core.dll => VKD3D-Proton (the DX12 path FH6 needs).
# dxgi.dll + d3d11.dll only  => DXVK, i.e. NOT the DX12 path.
run translation-layers "DXVK / VKD3D-Proton / Wine / Box64 present in containers" '
for base in /sdcard/Winlator /storage/emulated/0/Winlator /storage/emulated/0/GameHub \
            /storage/emulated/0/GameNative /data/data/com.winlator/files; do
  [ -d "$base" ] || continue
  echo "== $base"
  find "$base" -maxdepth 8 \
    \( -name "d3d12.dll" -o -name "d3d12core.dll" -o -name "dxgi.dll" \
       -o -name "d3d11.dll" -o -name "d3d9.dll" \
       -o -name "wine" -o -name "wineserver" -o -name "box64" -o -name "box86" \) \
    2>/dev/null | head -60
done'

run layer-versions "Version strings inside translation-layer binaries" '
for base in /sdcard/Winlator /storage/emulated/0/Winlator /storage/emulated/0/GameHub \
            /storage/emulated/0/GameNative; do
  [ -d "$base" ] || continue
  find "$base" -maxdepth 8 \( -name "d3d12core.dll" -o -name "dxgi.dll" -o -name "box64" \) 2>/dev/null |
  while read -r f; do
    echo "== $f"
    strings "$f" 2>/dev/null | grep -Eio "(vkd3d[-a-z]*|dxvk|box64)[ -]?v?[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -u | head -5
  done
done
command -v box64 >/dev/null 2>&1 && box64 -v 2>&1 | head -3
command -v wine >/dev/null 2>&1 && wine --version 2>&1 | head -3
:'

# ------------------------------------------------------------------
# CPU -- Oryon topology. Needed before any affinity work in Stage 6.
# ------------------------------------------------------------------
run cpu-topology "CPU cores, clusters, frequencies, governors" '
echo "== /proc/cpuinfo (trimmed)"
grep -Ei "processor|model name|BogoMIPS|CPU part|CPU implementer" /proc/cpuinfo 2>/dev/null | head -60
echo
echo "== per-core cpufreq"
printf "%-6s %-12s %-12s %-12s %-16s %s\n" cpu cur_kHz min_kHz max_kHz governor siblings
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  n=$(basename "$c")
  printf "%-6s %-12s %-12s %-12s %-16s %s\n" "$n" \
    "$(cat "$c/cpufreq/scaling_cur_freq" 2>/dev/null || echo -)" \
    "$(cat "$c/cpufreq/cpuinfo_min_freq" 2>/dev/null || echo -)" \
    "$(cat "$c/cpufreq/cpuinfo_max_freq" 2>/dev/null || echo -)" \
    "$(cat "$c/cpufreq/scaling_governor" 2>/dev/null || echo -)" \
    "$(cat "$c/topology/core_siblings_list" 2>/dev/null || echo -)"
done'

# ------------------------------------------------------------------
# Thermal -- Stage 6/7 need the baseline before load.
# ------------------------------------------------------------------
run thermal "Thermal zones (idle baseline)" '
for z in /sys/class/thermal/thermal_zone*; do
  t=$(cat "$z/type" 2>/dev/null); v=$(cat "$z/temp" 2>/dev/null)
  [ -n "$t" ] && printf "%-28s %s\n" "$t" "$v"
done | sort'

run memory "RAM and swap" 'cat /proc/meminfo 2>/dev/null | head -20; echo; cat /proc/swaps 2>/dev/null'

run storage "Storage backing the game install" 'df -h 2>/dev/null | head -20'

# ------------------------------------------------------------------
# Logs
# ------------------------------------------------------------------
run logcat "Recent logcat (graphics/wine/vulkan filtered)" \
  'logcat -d -v time 2>/dev/null | grep -Ei "vulkan|adreno|kgsl|turnip|freedreno|wine|box64|dxvk|vkd3d|winlator|gpu" | tail -400 || echo "logcat unavailable"'

run dmesg-gpu "Kernel messages mentioning the GPU" \
  'dmesg 2>/dev/null | grep -Ei "kgsl|adreno|gpu|drm" | tail -200 || echo "dmesg unavailable (needs root on most devices)"'

# ------------------------------------------------------------------
# Summary
# ------------------------------------------------------------------

# Strip the "###" provenance header before matching. Without this, a grep for
# a filename matches the echoed command line and reports a find that never
# happened -- a false positive is worse than no answer here.
body() { sed '/^### /d' "$OUT/$1.txt" 2>/dev/null; }

{
  echo "star-bionic diagnose summary"
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "mode: $MODE"
  echo
  echo "== identity =="
  body device-props | grep -Ei "ro\.product\.model|ro\.board\.platform|ro\.soc\.model|ro\.build\.version\.release" \
    | sed 's/^/  /' | grep . || echo "  (unavailable)"
  echo
  echo "== GPU (from KGSL, not inferred) =="
  body gpu-kgsl | grep -E "^gpu_model|^max_gpuclk" | sed 's/^/  /' | grep . || echo "  (unavailable)"
  echo
  echo "== DX12 path present? =="
  if body translation-layers | grep -qi "d3d12core\.dll"; then
    echo "  d3d12core.dll FOUND -> a VKD3D-Proton (DX12) path exists in a container"
  else
    echo "  d3d12core.dll NOT found -> no VKD3D-Proton DX12 path detected."
    echo "  FH6 is DX12-only; without this the game cannot render."
  fi
  echo
  echo "== Turnip present? =="
  if body vulkan-icd | grep -v "^== " | grep -qiE "/[^ ]*(freedreno|turnip)"; then
    echo "  freedreno/turnip artifacts FOUND on disk"
    echo "  NOTE: presence on disk does not mean it is the driver in use."
    echo "  Confirm with: vkprobe --icd <that json>  and read driver_properties.driverName"
  else
    echo "  no Turnip artifacts found -> likely running the Qualcomm blob"
  fi
  echo
  echo "== next step =="
  echo "  Run vkprobe (tools/vkprobe) to capture actual Vulkan capabilities."
  echo "  Nothing in Stage 3+ should be designed before that JSON exists."
} > "$OUT/SUMMARY.txt"

tar -czf "$OUT.tar.gz" "$OUT" 2>/dev/null && echo "---------------------------------------------" && echo "archive: $OUT.tar.gz"
echo
cat "$OUT/SUMMARY.txt"
