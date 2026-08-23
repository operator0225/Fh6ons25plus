#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# star-bionic collect -- one command, run in Termux on the S25+.
#
# Installs what it needs, runs the Stage 1 device capture, builds and runs the
# Stage 2 Vulkan probe against every driver it can find, and bundles the lot
# into a single tarball.
#
#   ./tools/collect.sh              # collect, print how to send results back
#   ./tools/collect.sh --push       # also commit+push to a results branch
#
# Nothing here needs root, adb, or an inbound network connection. The phone
# only ever makes outbound HTTPS.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO_ROOT/collect-$STAMP"
DO_PUSH=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --push) DO_PUSH=1; shift ;;
    -h|--help) sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "collect.sh: unknown argument '$1'" >&2; exit 2 ;;
  esac
done

mkdir -p "$OUT"
echo "star-bionic collect -> $OUT"
echo "================================================"

# ------------------------------------------------------------------
# 0. Dependencies (Termux)
# ------------------------------------------------------------------
if command -v pkg >/dev/null 2>&1; then
  echo "[0/4] installing Termux packages (clang, vulkan-headers, vulkan-tools)"
  # Not fatal: the probe still builds if only clang lands, and diagnose.sh
  # needs nothing at all.
  pkg install -y clang vulkan-headers vulkan-tools git tar >/dev/null 2>&1 \
    || echo "      (some packages failed; continuing)"
else
  echo "[0/4] no Termux 'pkg' -- assuming dependencies are already present"
fi

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || CC=clang

# ------------------------------------------------------------------
# 1. Stage 1 device capture
# ------------------------------------------------------------------
echo "[1/4] device + stack capture"
( cd "$OUT" && bash "$REPO_ROOT/tools/diagnose.sh" --out diag >/dev/null 2>&1 ) \
  && echo "      ok -> diag/" \
  || echo "      diagnose.sh returned nonzero (partial capture kept)"

# ------------------------------------------------------------------
# 2. Build vkprobe
# ------------------------------------------------------------------
echo "[2/4] building vkprobe"
if "$CC" -O2 -std=c11 -o "$OUT/vkprobe" "$REPO_ROOT/tools/vkprobe/vkprobe.c" -ldl \
     2> "$OUT/vkprobe-build.log"; then
  echo "      ok"
else
  echo "      BUILD FAILED -- see vkprobe-build.log"
  echo "      (usually means vulkan-headers is missing: pkg install vulkan-headers)"
fi

# ------------------------------------------------------------------
# 3. Probe every driver we can find
# ------------------------------------------------------------------
echo "[3/4] probing drivers"
if [[ -x "$OUT/vkprobe" ]]; then
  # 3a. whatever the loader picks by default -- normally the Qualcomm blob
  if "$OUT/vkprobe" > "$OUT/caps-default.json" 2> "$OUT/caps-default.err"; then
    name=$(grep -o '"driverName": "[^"]*"' "$OUT/caps-default.json" | head -1 | cut -d'"' -f4)
    echo "      default loader   -> ${name:-unknown}"
  else
    echo "      default loader   -> FAILED (see caps-default.err)"
  fi

  # 3b. every Turnip/Mesa ICD present on the device.
  # Presence on disk is not use -- each one gets probed explicitly so
  # driverName in the output says which driver actually answered.
  mapfile -t ICDS < <(find /data /sdcard /storage/emulated/0 -maxdepth 8 \
      \( -name "*freedreno*icd*.json" -o -name "*turnip*.json" -o -name "*_icd*.json" \) \
      2>/dev/null | head -12)

  if [[ ${#ICDS[@]} -eq 0 ]]; then
    echo "      no Mesa/Turnip ICD json found on device"
  fi

  i=0
  for icd in "${ICDS[@]}"; do
    i=$((i+1))
    dest="$OUT/caps-icd$i.json"
    echo "$icd" > "$OUT/caps-icd$i.path"
    if "$OUT/vkprobe" --icd "$icd" > "$dest" 2> "$OUT/caps-icd$i.err"; then
      name=$(grep -o '"driverName": "[^"]*"' "$dest" | head -1 | cut -d'"' -f4)
      echo "      icd$i -> ${name:-unknown}   ($icd)"
    else
      echo "      icd$i -> FAILED   ($icd)"
    fi
  done
else
  echo "      skipped (no vkprobe binary)"
fi

# ------------------------------------------------------------------
# 4. Bundle
# ------------------------------------------------------------------
echo "[4/4] bundling"
rm -f "$OUT/vkprobe"          # binary is not worth shipping back
TARBALL="$REPO_ROOT/star-bionic-collect-$STAMP.tar.gz"
tar -czf "$TARBALL" -C "$REPO_ROOT" "$(basename "$OUT")" 2>/dev/null

{
  echo "star-bionic collection"
  echo "collected: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "== drivers probed =="
  for f in "$OUT"/caps-*.json; do
    [ -e "$f" ] || continue
    n=$(grep -o '"driverName": "[^"]*"' "$f" | head -1 | cut -d'"' -f4)
    d=$(grep -o '"driverID": "[^"]*"' "$f" | head -1 | cut -d'"' -f4)
    bc=$(grep -o '"textureCompressionBC": [a-z]*' "$f" | head -1 | awk '{print $2}')
    printf "  %-22s %-26s %s  textureCompressionBC=%s\n" \
      "$(basename "$f")" "${n:-?}" "${d:-?}" "${bc:-?}"
  done
  echo
  cat "$OUT/diag/SUMMARY.txt" 2>/dev/null
} > "$OUT/COLLECT-SUMMARY.txt"

cat "$OUT/COLLECT-SUMMARY.txt"
echo "================================================"
echo "tarball: $TARBALL"
echo "size:    $(du -h "$TARBALL" 2>/dev/null | cut -f1)"

# ------------------------------------------------------------------
# Optional: push results back through GitHub over HTTPS
# ------------------------------------------------------------------
if [[ $DO_PUSH -eq 1 ]]; then
  echo
  echo "pushing results branch..."
  branch="results/s25plus-$STAMP"
  (
    cd "$REPO_ROOT" || exit 1
    git checkout -b "$branch" 2>/dev/null || git checkout "$branch" || exit 1
    # -f: collect-*/ is gitignored by design, but results are the point here.
    git add -f "$(basename "$OUT")" || exit 1
    git -c user.name="star-bionic-collect" \
        -c user.email="collect@localhost" \
        commit -q -m "device collection $STAMP (S25+ / Adreno 830)" || exit 1
    git push -u origin "$branch"
  ) && echo "pushed: $branch" || {
    echo "push failed."
    echo "  Termux needs GitHub credentials over HTTPS -- a personal access"
    echo "  token works: git remote set-url origin \\"
    echo "    https://<user>:<token>@github.com/operator0225/Fh6ons25plus"
    echo "  Or skip --push and just send the tarball above."
  }
else
  echo
  echo "Send that tarball back, or re-run with --push to push a results branch."
fi
