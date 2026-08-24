# Star Bionic

Adreno 830 / Snapdragon 8 Elite graphics optimisation for running Forza
Horizon 6 (DX12) under a Winlator-family runtime on Android.

Target device: Galaxy S25+ · Snapdragon 8 Elite · Adreno 830

```
FH6 (x86_64, DX12)
  → Wine/Proton  → Box64 → ARM64 / Oryon
  → VKD3D-Proton → Vulkan → Star Bionic → Turnip → Adreno 830
```

---

## Status: Stage 1–2 tooling. No optimisation exists yet.

This repository was **empty** at the start of this work — zero commits. There
was no Winlator fork to analyse and no existing stack to optimise. What
exists now is the measurement tooling that everything else depends on.

**No performance claim is made anywhere in this repository.** Nothing has
been run on an Adreno 830. Whether 1080p / High / 60 FPS is achievable is an
open question, and it will be answered by measurement or not at all.

**Game acquisition is separately blocked.** The available FH6 copy is PC Game
Pass, and no Game Pass title currently launches outside Windows — Xodus can
sign in, download and decrypt, but cannot launch games pending an upstream
Xbox Gaming Runtime reimplementation with no announced timeline. See
[docs/01-gamepass-msstore.md](docs/01-gamepass-msstore.md). This blocks
launching FH6 specifically; it does not block Stages 2-12, which depend on the
device and translation stack rather than on the storefront.

| Stage | State |
|---|---|
| 1 — Environment survey | **done** — S25+ / SM8750 / Adreno830v2 confirmed |
| 2 — Adreno 830 capabilities | **measured** — [vendor driver](docs/02-adreno830-capabilities.md) and [Turnip in-container](docs/03-turnip-capabilities.md) |
| 3 — Star Bionic layer | not started — target list now set by Stage 2 |
| 4 — VKD3D-Proton / DX12 path | not started |
| 5 — Shader & pipeline cache | not started |
| 6 — CPU affinity | not started |
| 7 — Android overhead | not started |
| 8–9 — Resolution / dynamic res | not started |
| 10 — Profiler | **system-side done** — [docs](docs/10-profiler.md); FPS needs in-container capture |
| 11 — Benchmark harness | not started |
| 12 — Optimisation priority | n/a |
| 13 — Feature flags | [contract defined](docs/13-feature-flags.md) |

Stage 2 is measured on both drivers. **Every VKD3D-Proton requirement is
present on each, including `textureCompressionBC`**, so capability is not the
blocker. The binding constraint measured so far is **GPU memory: Turnip
reports a live budget of 2.80 GiB**, dynamic, against a title that expects
desktop VRAM. The other open hypothesis is that Turnip exposes **a single
universal queue**, so D3D12's copy and compute queues serialise onto the
graphics queue — plausible frame-time cost, not yet measured.

---

## Read first

- **[docs/00-repo-analysis.md](docs/00-repo-analysis.md)** — Stage 1 report:
  empty-repo finding, build environment limits, verified external facts.
- **[docs/01-gamepass-msstore.md](docs/01-gamepass-msstore.md)** — **FH6 from
  a Game Pass copy cannot currently be launched outside Windows.** The
  Microsoft Store cannot be added to the build, and Xodus cannot launch games
  yet. The blocker is upstream; the graphics work is unaffected.
- **[docs/02-adreno830-capabilities.md](docs/02-adreno830-capabilities.md)** —
  Stage 2 results measured on the device: what Adreno 830 actually exposes,
  the gaps that shape the plan, and what it does not tell us.
- **[docs/03-turnip-capabilities.md](docs/03-turnip-capabilities.md)** —
  **Turnip measured inside Winlator**, the driver the stack actually runs on.
  Overturns two Stage 2 conclusions, and finds a live GPU memory budget of
  2.80 GiB plus a single-queue limitation.
- **[docs/13-feature-flags.md](docs/13-feature-flags.md)** — flag contract.

---

## Tools

### `tools/diagnose.sh` — device & stack capture

```sh
./tools/diagnose.sh --out diag-s25plus     # on-device (Termux)
./tools/diagnose.sh --adb --out diag-s25plus   # from a host over adb
```

Captures SoC identity, KGSL GPU state, CPU topology and governors, thermal
baseline, memory, Vulkan ICDs, Winlator-family packages, container contents,
translation-layer versions, and filtered logcat/dmesg. Produces a directory,
a tarball, and a `SUMMARY.txt`.

Answers the two gating questions: is a VKD3D-Proton DX12 path present, and is
Turnip actually the driver in use?

### `tools/collect.sh` — one-command device collection

Wraps both tools below for Termux: installs dependencies, captures the
device, builds `vkprobe`, probes the default loader *and* every Mesa/Turnip
ICD found on disk, and bundles a tarball with a summary listing each
driver's identity and `textureCompressionBC`. This is the intended entry
point; the two tools below are what it drives.

### `tools/report.sh` — paste-able Stage 2 summary

```sh
./tools/report.sh collect-*/caps-android.json
```

Renders a capture as a compact table: driver identity, API level, the
DX12/VKD3D-critical feature set, texture and depth formats, subgroup, sparse,
memory heaps and limits. Needs only grep/awk. A field the probe did not
capture prints `?`, never a guessed `no`.

### `tools/profiler.py` — Stage 10 sampler

```sh
termux-wake-lock                      # or Android suspends backgrounded Termux
./tools/profiler.py --duration 300 --out runs/baseline --label "idle"
```

Samples CPU load and frequency per core, GPU load/frequency/temperature/
throttling from KGSL, thermal zones, RAM and swap. Writes CSV plus a JSON
summary with min/mean/p50/p95/max. Prints which sources are actually readable
before sampling starts, and writes an empty cell — never a 0 — for anything
it could not read. See [docs/10-profiler.md](docs/10-profiler.md).

FPS, frametime and shader/pipeline timing are **not** included: they exist
only inside the rendering process. The doc explains the routes to them.

### `tools/vkprobe/` — Vulkan capability probe

Reports what Vulkan actually exposes, from `vkGetPhysicalDeviceFeatures2` /
`Properties2` / `FormatProperties2`. Covers the full Stage 2 checklist. JSON
output. See [tools/vkprobe/README.md](tools/vkprobe/README.md).

Never fakes an extension, never conflates "not queried" with "unsupported",
and reports which driver actually answered.

---

## Next step — run `collect.sh` on the S25+

One command in Termux. Installs what it needs, runs the Stage 1 capture,
builds and runs the Stage 2 probe against **every** driver it finds, and
bundles one tarball:

```sh
pkg install -y git
git clone https://github.com/operator0225/Fh6ons25plus
cd Fh6ons25plus && git checkout claude/star-bionic-adreno-fh6-8a79z1
./tools/collect.sh              # or --push to push a results branch
```

No root, no adb, no inbound connection — the phone only makes outbound
HTTPS. Send the tarball back, or use `--push`.

The summary it prints already answers the two questions that gate Stage 3:
which driver actually answered, and whether `textureCompressionBC` is true.

Stage 2 analysis and the Stage 3 design follow from that data.

---

## Ground rules

Carried from the project spec, and enforced in the tooling rather than just
documented:

- Never assume a Vulkan extension exists — query it.
- Never assume Adreno 830 support — measure it.
- Never treat a number from the internet as this device's performance.
- Never share a shader cache across differing GPU/driver/Vulkan/VKD3D/game
  versions.
- Never enable a GPU-hang-risking workaround by default.
- Never claim "optimised" without a before/after benchmark.
- Never claim 1080p60 is guaranteed.
- Correctness is never traded for performance.
