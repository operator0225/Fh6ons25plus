# vkprobe — Vulkan capability probe (Stage 2)

Reports what a Vulkan implementation **actually** exposes. Every answer comes
from `vkGetPhysicalDeviceFeatures2` / `Properties2` / `FormatProperties2`.
Nothing is inferred from the GPU name or from documentation.

## Build

**On-device via Termux** (usually easiest — no host SDK, and it runs where
you are measuring):

```sh
pkg install clang vulkan-headers
cc -O2 -o vkprobe vkprobe.c -ldl
```

**Cross-compile with the NDK:**

```sh
ANDROID_NDK_HOME=/path/to/ndk ./build-android.sh
adb push build/arm64-v8a/vkprobe /data/local/tmp/
adb shell chmod 755 /data/local/tmp/vkprobe
```

## Run

```sh
vkprobe > caps.json                       # whichever driver the loader picks
vkprobe --icd /path/to/freedreno_icd.aarch64.json > caps-turnip.json
vkprobe --lib /path/to/libvulkan.so       # non-default loader
vkprobe --device 0                        # single device
```

JSON goes to stdout, diagnostics to stderr, so `vkprobe > caps.json` always
yields valid JSON.

## Capture both drivers

The point of Stage 2 is knowing which driver you are actually on. Capture
both and diff them:

```sh
vkprobe > caps-system.json                                  # Qualcomm blob
vkprobe --icd /path/to/freedreno_icd.aarch64.json > caps-turnip.json
```

Then confirm which answered:

```sh
jq '.physical_devices[0].driver_properties |
    {driverID, driverName, driverInfo, is_turnip}' caps-turnip.json
```

If `is_turnip` is `false` in a file you believe is Turnip, **the numbers in
that file are the wrong driver's numbers.** Fix the ICD path before drawing
any conclusion from it.

> **AdrenoTools caveat:** a standalone binary uses the loader visible to *its
> own* process. A custom driver injected into another app by AdrenoTools is
> **not** picked up here. Use `--icd` to point at the same driver file, or run
> the probe inside the same container — otherwise you are measuring the
> system driver while believing you are measuring Turnip.

## Reading the output

Every capability block carries a `__source` field:

| `__source` | meaning |
|---|---|
| `core1.2` / `core1.3` | queried via the promoted core struct |
| `VK_EXT_...` | queried via that extension's struct |
| `not-available` | **not queried at all** — device lacks it entirely |

`not-available` is not the same as `false`. A block marked `not-available`
contains no capability keys, so a missing answer can never be misread as a
negative one.

### Fields that matter most

| Field | Why |
|---|---|
| `driver_properties.driverName` | which driver actually answered |
| `features_core_1_0.textureCompressionBC` | see below |
| `d3d12_relevant.*` | the caps a D3D12 translation layer leans on |
| `formats.BC7_UNORM.supported` | FH6's texture format, concretely |
| `pipeline_cache.pipelineCacheUUID` | must be in the Stage 5 cache key |
| `memory.heaps[].size` | the real budget for a 150 GB-install game |

**`textureCompressionBC` is the one to look at first.** Every DX12 PC title
ships BCn textures. Desktop GPUs support them natively; mobile GPUs
historically do not, favouring ASTC/ETC2. If BC is absent, VKD3D-Proton must
transcode or decompress, at large CPU and memory cost — which would shape the
entire optimisation strategy well before any of the Vulkan-overhead work in
Stage 3 becomes relevant.

`d3d12_relevant` is **informational**. It reports caps a D3D12 layer
generally depends on, but the authoritative requirement list is whatever the
VKD3D-Proton build pinned in Stage 4 checks at runtime. Do not read all-true
as "FH6 will run", and do not read a false as "impossible" without checking
against that build.

## Validation status

Compile-tested clean with `-Wall -Wextra` and validated end-to-end against
lavapipe on x86_64: correct driver identification, correct `__source`
provenance on every block, correct format detection, and the gating
invariant verified (no `not-available` block carries data keys).

**It has never been run on an Adreno GPU.** It is a working instrument that
has not yet taken the measurement.
