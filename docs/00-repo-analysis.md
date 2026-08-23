# Stage 1 — Repository & Environment Analysis

Date: 2026-08-23

Stage 14 requires the repository be analysed and reported before any work
starts. This is that report.

---

## 1. Repository state

**The repository is empty.** Not sparse — empty.

```
$ git log --oneline
fatal: your current branch 'claude/star-bionic-adreno-fh6-8a79z1' has no commits yet
$ ls -A
.git
```

Zero commits, zero files, zero branches on the remote.

This means every item on the Stage 1 checklist — Winlator fork structure,
Wine/Proton version, Box64 version, DXVK, VKD3D-Proton, Vulkan loader, Mesa,
Turnip, Adreno driver, shader cache, pipeline cache, memory allocator,
synchronization, command submission, descriptor management, Android surface
integration, Wayland/X11/SurfaceFlinger path, CPU affinity, GPU frequency
handling, thermal throttling — **is not present and cannot be analysed.**
There is no existing code to study. This is a greenfield start, not a fork
being optimised.

Consequence for the plan: Stage 1's "analyse the existing stack" cannot be
done from the repository. It has to be done **against the actual device**,
which is what `tools/diagnose.sh` is for.

---

## 2. Build environment available to this session

| | |
|---|---|
| Host | x86_64 Linux VM, 4 cores, 15 GB RAM, ~30 GB free |
| Present | gcc 13, clang 18, cmake 3.28, ninja, python 3.11, rust, node |
| **Missing** | meson, aarch64 cross-compiler, Android NDK/SDK, adb, glslang, vulkaninfo |
| **Missing** | any Adreno GPU, any Android device, any Snapdragon CPU |

Vulkan headers (1.3.275) and a software Vulkan driver (lavapipe) were
installed here **solely to compile-test and validate tooling**. They are not
part of the product.

### What this environment can and cannot do

**Can:** write and compile-test C/shell tooling, design cache formats and
flag contracts, cross-check facts, prepare on-device scripts.

**Cannot:** build Mesa/Turnip for ARM64 (no NDK, no meson), run anything on
Adreno 830, or measure a single real frame.

Every performance number in this project must therefore come from the user's
S25+. Nothing measured on this VM is device performance, and nothing in this
repository will claim otherwise.

---

## 3. External facts verified (2026-08-23)

These were checked against current sources because they postdate the model's
training data and each one changes the plan.

### Forza Horizon 6
Released **19 May 2026** on **both Steam and PC Game Pass / Microsoft Store**.
DX12 is a minimum requirement, along with an SSD and ~150 GB.

The dual availability matters enormously — see `01-gamepass-msstore.md`.

### Turnip on Adreno 830
- Mesa 26.0 landed **initial** Adreno Gen 8 support.
- Turnip 26.1.0 + the A8XX v20+ driver line added proper **Adreno 830**
  configuration around mid-2026, reportedly fixing significant GMEM issues.
- A Mesa GitLab comment flagged **missing kernel support** for A830 at one
  point; whether that still applies on this specific device/kernel is
  unverified.
- Practical builds come from third-party CI (StevenMXZ, whitebelyash,
  Banners-Turnip), not from a distro package.

**This is reported status, not verified capability.** Per project rule 4,
Turnip's actual support on *this* device gets confirmed by running
`tools/vkprobe` against the Turnip ICD and reading `driver_properties` — not
by trusting the above.

### Winlator ecosystem
Winlator 11 is current, alongside forks: CMOD, GameNative, GameHub, WinHub.
All share the same stack shape: Box64 → Wine → DXVK/VKD3D-Proton → Turnip.
No fork has been selected yet; that decision should follow the Stage 1
measurements, not precede them.

---

## 4. What was built in this increment

Two tools, both validated:

### `tools/diagnose.sh` — Stage 1 device capture
Captures SoC identity, KGSL GPU state, CPU topology and governors, thermal
baseline, memory, Vulkan ICDs on disk, installed Winlator-family packages,
container contents, translation-layer versions, and filtered logcat/dmesg.
Runs on-device or over adb. Emits a directory, a tarball, and a `SUMMARY.txt`
that answers the two questions that gate everything else:

- Is there a VKD3D-Proton (DX12) path present at all?
- Is Turnip actually present — and is it actually the driver in use?

### `tools/vkprobe/` — Stage 2 capability probe
A C program reporting what Vulkan *actually* exposes, from
`vkGetPhysicalDeviceFeatures2` / `Properties2` / `FormatProperties2`. Covers
the full Stage 2 list. Outputs JSON.

Three design rules enforced in code:

1. An extension-gated struct is chained **only** if the device advertises
   that extension or core promoted it. Otherwise the driver never writes to
   it and we would report our own zero-init as a driver answer.
2. Every block carries `__source` naming where the answer came from
   (`core1.3`, `VK_EXT_mesh_shader`, `not-available`). "Not queried" is never
   collapsed into "returned false".
3. The loader is `dlopen`'d and `--icd` selects a driver explicitly, so you
   can tell Turnip from the Qualcomm blob and confirm which one answered.

**Validated locally against lavapipe**: correct driver ID, correct provenance
on every block, correct format detection, and the gating invariant holds
(no `not-available` block carries data). The probe works; it has simply never
seen an Adreno.

An instructive contrast from that run: lavapipe reports **11/11 BC formats
and 0/4 ASTC**. A mobile GPU will likely show the inverse. Which is exactly
the point — `textureCompressionBC` is the single most consequential unknown
for a DX12 title on Adreno, because every FH6 texture ships as BCn. If BC is
absent, VKD3D-Proton must transcode, at significant CPU and memory cost.
That question is now measurable instead of arguable.

---

## 5. What was deliberately NOT built

Stages 3–12 were left alone on purpose. Star Bionic's optimisation layer,
the profiler, the benchmark harness, dynamic resolution, and CPU affinity
tuning all depend on measurements that do not exist yet. Building them now
would mean guessing at Adreno 830's capabilities — which project rules 4, 5
and 6 explicitly prohibit.

The one exception is the feature-flag contract (`13-feature-flags.md`),
defined early because it is a design constraint on everything that follows,
not an optimisation in itself.

---

## 6. Next step

Run on the S25+:

```sh
./tools/diagnose.sh --out diag-s25plus
# then, per tools/vkprobe/README.md, build and run vkprobe against BOTH
# the system driver and the Turnip ICD
```

Send back `diag-s25plus.tar.gz` and both `*.json` files. Stage 2 analysis and
the Stage 3 design follow from those, and not before.
