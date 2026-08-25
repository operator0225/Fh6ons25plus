# Runtime selection — which Winlator, and what to change

Date: 2026-08-24

## What is already installed, inferred from measurement

The Turnip capture reports **`Mesa 26.1.0-devel`**. Official Winlator **11.0**
bundles exactly `Mesa Turnip v26.1.0 devel`. Nothing else in the ecosystem
publishes that pairing.

So the running stack is almost certainly **official Winlator 11.x
(brunodev85)** — confirm with:

```sh
./tools/star-bionic-run runtime
```

That matters because the choice is not "pick a fork from a blog table". It is
"is there a specific, evidenced reason to change a stack this project has
already characterised in detail?"

## The landscape, from primary sources

Component versions below come from the projects' own GitHub releases, not
from comparison articles — several of which gave mutually contradictory dates
and are not cited here.

There are **two separate lineages**, and their version numbers are not
comparable. "Ludashi 3.1" is not older than "Winlator 11.2"; they are
different projects counting separately.

```
brunodev85/winlator  ──────────────────────►  official 11.x   (glibc + proot, x86_64/Box64)
        │
        └──► coffincolors  (Cmod v13.x)
                    │
                    └──► Pipetto-crypto  (Bionic: +Arm64EC, +FEXCore)
                                │
                                └──► StevenMXZ  (Winlator-Ludashi 3.x)
```

| Runtime | Wine | CPU translation | Turnip | State |
|---|---|---|---|---|
| **Winlator 11.2 Beta** (official) | — | Box64 **v0.4.4** | — | current beta |
| **Winlator 11.0** (official) | 10.10 | Box64 v0.4.0 | **26.1.0-devel + whitebelyash A8XX patches** | what we measure |
| Winlator 10.1 | — | Box64 v0.3.6 | 25.1.0 | superseded |
| Winlator Cmod v13.1.1 | 9.20 / Proton 10 | Box64 | bundled | older base |
| **Winlator-Ludashi 3.1.h** | — | Box64 **or FEXCore (Arm64EC)** | "new default, supports A8xx" | active, Cmod→Bionic line |
| GameHub / GameNative / WinHub | — | — | — | separate lineage |

Notes worth having:

- **Correction.** An earlier version of this document claimed no fork's
  release notes mention Adreno 8xx. That was **wrong**, and wrong in the
  direction that mattered. Official Winlator 11.0's own notes read *"Updated
  Mesa Turnip (v26.1.0 devel with whitebelyash patches for A8XX)"* — so the
  stack already in use carries **Adreno-8xx-specific patches**, which is why
  an A830 works at all. Ludashi 3.1.h separately says *"new default turnip
  which also supports adreno A8xx"*. Both mention it; the claim was simply
  false.
- That correction **strengthens** the recommendation rather than weakening it.
  Ludashi's note is a fork *catching up* to A8xx support that official 11.0
  already shipped. It is not evidence of a newer or better Turnip — Ludashi
  publishes no Turnip version number to compare against.
- The widely-linked `Stredohori/Winlator-CMOD` mirror was **archived in May
  2026**; Cmod releases come from coffincolors directly. Its published base is
  Wine 9.20 / Proton 10 era — **older than official 11.0's Wine 10.10**.
- Pipetto-crypto, the Bionic upstream, publishes **no releases at all** — the
  repo's Releases page is empty, and community reports say the original
  developers moved on. The APKs come from downstream forks such as Ludashi,
  which is actively maintained (3.1.h, Jun 22).
- Reliable release *dates* could not be established. Sources conflict badly
  and much of what is indexed is content-farm material. Version-to-component
  mappings from GitHub releases are used instead, and those are consistent.

## Recommendation

**Stay on official Winlator. Do not switch forks.**

The reasoning is evidence, not preference:

1. **The GPU side is already handled.** The measured Turnip carries the
   whitebelyash A8XX patches, every VKD3D-Proton requirement is present, and
   BC textures are native. No fork publishes a Turnip version that can be
   *shown* to be newer — Ludashi ships one without naming it, so switching for
   the GPU would be trading a measured driver for an unnamed one.
2. **Cmod's published base is older** — Wine 9.20 against 11.0's 10.10. Moving
   there is a downgrade on the axis that is actually unmeasured.
3. **Switching forks means rebuilding the container**, discarding a
   configuration this project has measured across ten rounds. That is a
   regression risk taken against a stack known to work.

This is a recommendation about the **GPU** axis, which is the axis measured so
far. The section below is the one open case against it.

### The one targeted upgrade

**11.0 → 11.2 Beta, for Box64 v0.4.4 (from v0.4.0).**

Box64 is the x86_64→ARM64 translation layer, and it is the single largest
unmeasured factor in this project: the GPU has demonstrated headroom, so CPU
translation is the plausible next bottleneck. A dynarec improvement is the
one change with a specific reason behind it.

It is a beta. Keep the 11.0 APK so it can be rolled back, and re-run
`vkbench` after upgrading — the numbers are directly comparable now that
`thermal` gates the starting state.

### The open case for Ludashi: Arm64EC + FEXCore

**Official Winlator has no Arm64EC container type and no FEXCore.** The
Bionic line does, and that is the one capability official 11.x genuinely
lacks. It is worth taking seriously for a reason that has nothing to do with
Turnip:

> In an Arm64EC container, Wine's own DLLs — including **VKD3D-Proton** —
> run as native ARM64 code. Only the game's x86_64 code is translated.

Under Box64 on official Winlator, *everything* is translated, VKD3D's DX12→
Vulkan work included. For a DX12-only title that is a real amount of work
moved off the translator. That aims squarely at the factor this project has
already named as the largest unmeasured one and the plausible next
bottleneck.

**It is a hypothesis, not a recommendation.** What makes it untested:

- The game's own code is still emulated — by FEXCore instead of Box64. Which
  translator is faster *for this game* is unknown, and swapping Box64 for
  FEXCore is not automatically a win.
- No Wine, FEXCore or Turnip version numbers are published for Ludashi
  3.1.h, so there is nothing to compare against the measured stack.
- The Bionic upstream is unmaintained; only downstream forks ship builds.
- It requires a full reinstall and a rebuilt container, discarding the
  characterised configuration.
- FH6 [cannot be launched at all](01-gamepass-msstore.md) from the available
  Game Pass copy, so the end-to-end case cannot be closed either way yet.

The way to settle it is a CPU-side benchmark run under both — the same
measurement gap the Box64 tuning section refuses to guess around. Building
that turns this from an argument into a number.

### The package-name trick, and whether it applies here

Ludashi ships three APKs that are **the same build with different package
names**: `bionic-vanilla`, `ludashi-bionic` (impersonating the Ludashi
benchmark app), and `Redmagic-build`. The intent is to land in an OEM's
benchmark whitelist so the firmware unlocks a higher performance mode.

This is interesting here because **thermal throttling is this project's main
measured risk** — sustained clock is half of boost. Anything that moves the
sustained clock matters more than anything on the feature list.

But the reported effect is on **Xiaomi** devices, and the target here is a
Galaxy S25+. Samsung's equivalent mechanism is a different system, and
whether it responds to this package name on One UI in 2026 is **unverified**.
Do not assume it carries over.

It is, however, unusually easy to test properly: two APKs of the same build
differing *only* in package name is a controlled A/B. Run the `vkbench` soak
ladder from a `thermal`-verified cold start under each, and compare
`sustained_ms` and the profiler's GPU clock trace. That measures the claim
directly instead of repeating it.

### VKD3D-Proton

Swappable without changing runtime, via `.wcp` component packs
([Winlator-WCP-Collections](https://github.com/Nick088Official/Winlator-WCP-Collections),
which carries a vkd3d-proton collection). Those are aimed at Bionic-lineage
forks, so **verify they install on official Winlator before relying on it**.

Only worth doing with a reason — a specific FH6 bug fixed upstream, say.
Swapping the DX12 translation layer speculatively, on a stack whose DX12 path
is otherwise untested, adds a variable rather than removing one.

## Container configuration

Settings already validated on this device:

| Setting | Value | Why |
|---|---|---|
| Screen size | 1920x1080 | the target |
| Vulkan driver | **Turnip** | confirmed `MESA_TURNIP` in the capture |
| DX Wrapper / DirectX 12 | **VKD3D** | FH6 is DX12-only |
| Direct3D | DXVK | DX11 and below; unused by FH6 |
| HUD Mode | **Disabled** | it perturbs what it measures |

Cache environment, from measured cache behaviour — paste into the container's
Environment Variables:

```sh
./tools/star-bionic-run setup     # prints the block, keyed to this driver
```

That covers `MESA_SHADER_CACHE_DIR` (worth ~8x on its own),
`VKD3D_SHADER_CACHE_PATH` (vkd3d-proton's own DX12 cache) and
`DXVK_STATE_CACHE_PATH`, all under a key derived from `driverInfo` and
`pipelineCacheUUID` so a driver update cannot silently reuse a stale blob.

## Box64 tuning — candidates, not a recommendation

Box64 exposes dynarec tuning through `BOX64_DYNAREC_*` environment variables.
They are **deliberately not prescribed here**, because this project does not
call something an optimisation without a before/after benchmark, and the CPU
side has not been measured at all yet.

Setting them blind would be exactly the kind of cargo-cult configuration the
measurement work so far exists to avoid — and some of them trade correctness
for speed, which the Stage 12 priority list puts off the table.

The honest order is: build a CPU-side measurement first, establish a baseline,
then change one variable at a time against it. Until then the defaults stand.

## What would change this recommendation

- A fork shipping a **newer Turnip** than 26.1.0-devel with Gen 8 fixes — and
  publishing the version number, so the claim can be checked.
- **A measured Arm64EC/FEXCore win over Box64 on this device.** This is the
  strongest open case, and it becomes decidable the moment a CPU-side
  benchmark exists. Native-ARM64 VKD3D is a real structural advantage for a
  DX12 title; it is just not yet a measured one.
- **A measured sustained-clock gain** from the Ludashi package name on
  Samsung, via the A/B above.
- A **measured** Box64 regression in 0.4.4 versus 0.4.0.
- A specific FH6 defect fixed in a newer VKD3D-Proton.
- Wine gaining a Vulkan 1.4 instance — though on inspection the 1.3.301 cap
  costs nothing here, since every capability VKD3D needs is present as an
  extension rather than requiring 1.4 core.
