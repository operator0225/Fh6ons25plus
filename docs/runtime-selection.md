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
   BC textures are native.

   ~~No fork publishes a Turnip version that can be *shown* to be newer.~~
   **Retracted — see [Reading the source](#reading-the-source-rather-than-the-release-notes).**
   GameNative publishes 86 Turnip builds with exact version strings, several
   newer than the measured 26.1.0-devel, including Gen8- and OneUI-specific
   variants. That claim was made from release notes; the manifest says
   otherwise.
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

**Why not 11.1 Final instead?** Because taking the stable would gain nothing
here. 11.1's changes are a reworked gamepad implementation, Gladio (OpenGL),
D7VK in the DDraw wrapper, keyboard bindings and XAudio — **none of which are
on a DX12 title's path**. Box64 v0.4.4 first appears in 11.2. So the real
choice is "stay on 11.0, or take the beta", not "take the safe stable".

**A trap in reading that releases page:** GitHub's *Latest* badge is on **11.1
Final**, because 11.2 is flagged as a pre-release and pre-releases never get
the badge. 11.2 is the newer release. Anything reporting "11.1 is the newest
official Winlator" has read the badge rather than the list.

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

### GameNative — the strongest candidate, for a reason nobody was looking at

A survey of eight forks turned up one whose 1.2.0-prerelease notes (2026-08-19)
read:

> *"Add automatic CPU / GPU power control to AYN, Retroid Pocket and **Samsung
> Devices**, reduces heat, **thermal throttling** and power consumption"*

and separately:

> *"Fixed lsfg-vk on **8 elite** and other devices with frame capping"*

**That lands on this project's main measured risk, on this device family, by
name.** Sustained clock here is half of boost and frame-time spread collapses
from 0.2% to 44% under load — thermal is the number one finding, and this is
the only fork feature found so far that claims to act on it for Samsung
specifically. It outranks both the Arm64EC argument and the Ludashi
package-name trick, because those are inference and a Xiaomi report
respectively, while this names the vendor and the symptom.

It is also the fork most visibly aimed at this SoC: reported to detect
Snapdragon 8 Elite in code and branch to A8-Elite Turnip wrappers, Proton
10.0-arm64ec, and a VKD3D default, with FEXCore, Proton and VKD3D all
swappable as `.wcp`.

**Verify before relying on any of that:**

- ~~The component figures are internally inconsistent — 2.14.1 is not in the
  reported catalog.~~ **Resolved by reading the source: both numbers are real
  and live in two different catalogs.** See below.
- **It is a storefront launcher first.** The README is *"Play the PC games you
  already own — from Steam, Epic and GOG"*, with *"Log in to your Steam
  account"* as a setup step. 1.2.0 adds *"Support custom games on the modern
  build without all-files access"*, so local executables are possible — but
  this is not a general-purpose container app the way official Winlator is.
- **It does not unblock FH6.** The Game Pass copy still cannot launch
  anywhere; Steam/Epic/GOG integration is irrelevant to a Game Pass title, and
  [there is no Steam version available](01-gamepass-msstore.md).

So its value here is as a **thermal experiment and an Arm64EC test bed**, not
as a way to get the game running. Those are worth having — the power-control
claim is testable today with the existing soak ladder, profiler and `thermal`
gate, and it is a claim about the one thing measured to be limiting.

### Reading GPU clock and temperature from inside a container: closed

Surveying eight forks for a way to expose `/sys/class/kgsl` to the Windows
process found **none**. The features that look like it are all host-side:

- REF4IK's GPU/battery temperature is the **Android app** reading sensor paths
  and drawing them in its own UI.
- Bannerlator's Fusion HUD reads GPU/thermal/VRAM the same way.
- Bannerlator's FEX runtime indicator reads `/proc/<pid>/maps` — host-side
  inspection of the guest process, not guest access to sysfs.

This **confirms** the earlier measured finding rather than overturning it:
`kgsl_sysfs_readable: false` inside Winlator is the proot container's own root
filesystem, not a misconfiguration, and no fork fixes it. Thermal and clock
data has to keep coming from the profiler running in Termux alongside the
benchmark. The question is settled; stop looking for a fork that solves it.

### VKD3D-Proton

Swappable without changing runtime, via `.wcp` component packs
([Winlator-WCP-Collections](https://github.com/Nick088Official/Winlator-WCP-Collections),
which carries a vkd3d-proton collection). Those are aimed at Bionic-lineage
forks, so **verify they install on official Winlator before relying on it**.

Only worth doing with a reason — a specific FH6 bug fixed upstream, say.
Swapping the DX12 translation layer speculatively, on a stack whose DX12 path
is otherwise untested, adds a variable rather than removing one.

**An admitted gap in this document.** FH6 is DX12-only, so VKD3D-Proton is the
component that matters most — and **its version on official Winlator 11.2 is
unknown**. The 11.2 notes name Box64 v0.4.4 and say nothing about VKD3D; a
survey of eight forks could not establish it either, and most forks do not
publish a VKD3D version at all. So the upgrade recommended above is made on
the CPU component while the most important graphics component is unverified
on both sides of the comparison. `vkprobe` reports the driver, not the
translation layer; reading VKD3D's version out of a running container is not
yet covered by any tool here.

## Reading the source rather than the release notes

Everything above this point was built from release notes and third-party
summaries. Cloning the repositories and reading the manifests **corrected two
claims in this document and closed one admitted gap.** Release notes are
marketing; manifests are the shipping list.

### Component catalogues, from each repo's own manifest

| | official Winlator (`installable_components/`) | GameNative (`manifest.json` + `dxwrapper_download.json`) |
|---|---|---|
| **VKD3D-Proton** | 2.12, 2.14.1, **3.0b** | 2.6, 2.12, 2.13, 2.14.1, 3.0b, **3.0.1-0** |
| **Turnip** | 24.1.0, 25.0.0, **26.0.3** (bundle is 26.1.0-devel+A8XX) | **86 builds**, incl. 26.3.0-R3, Gen8 V34, **26.2.0_R4_OneUI** |
| **Box64** | 0.3.3, 0.3.5, 0.3.7 | 0.4.4 |
| **Proton** | — | 10.0 / 10.0-4 / 11.0-1, each **x86-64 and ARM64EC** |
| **FEXCore** | — | FEX 2601-217d039, 2607, 2608 |
| **WOWBox64** | — | 0.3.6, 0.4.4 |

### Corrections this forced

1. **"No fork publishes a newer Turnip" was wrong.** GameNative publishes 86
   with exact version strings. Three matter: `Turnip v26.3.0-R3` (newer than
   the measured 26.1.0-devel), `Turnip Gen8 V34` (an Adreno-8xx line, now at
   V34), and **`Turnip v26.2.0_R4_OneUI`** — a build named for Samsung's OS.
   Whether any is *better* here is unmeasured, but they are named, versioned
   and downloadable, which is exactly what the earlier claim denied.
2. **The "2.14.1 inconsistency" was not an inconsistency.** GameNative keeps
   *two* catalogues: `dxwrapper_download.json` (`.tzst`, tops out at 3.0b, and
   **does** contain 2.14.1) and `manifest.json` (`.wcp`, which is where
   3.0.1-0 lives). Both numbers were real; the survey had merged two files.
   Demanding the manifest was right; concluding "stale code path" was not.
3. **The VKD3D gap is now partly closed.** Official Winlator's catalogue is
   2.12 / 2.14.1 / **3.0b**. The version a container *defaults* to is still
   unread, but the ceiling is knowable and it is 3.0b.

### The most useful thing found: VKD3D 3.0b is installable on official Winlator

Upstream VKD3D-Proton's newest release is **3.0.1**, and its headline includes
performance work aimed at **mobile GPUs — deferred clears and render pass
suspend/resume.** Those are tiler optimisations: an Adreno resolves tiles to
memory on every render-pass boundary, so avoiding needless clears and keeping
a pass suspended across boundaries attacks the exact bandwidth cost that
dominates a mobile GPU.

That is the first concrete, non-speculative reason found for touching the DX12
layer — and it does **not** require changing forks. Official Winlator ships
3.0b in its own component list, one point release behind. Whether the gap
matters is measurable rather than arguable.

This also revises the earlier advice to leave VKD3D-Proton alone. That advice
was written when no specific reason existed. One now does; it is still a
one-variable change to be measured, not assumed.

### Samsung power control is a real SDK, not the package-name hack

`SamsungPerformanceDriver.kt` imports **`com.samsung.sdk.sperf`** — Samsung's
own Game Performance SDK — and calls `SPerf.initialize()`,
`PerformanceManager.getInstance()`, then `CustomParams.TYPE_CPU_MIN` and
siblings. It exposes **CPU, GPU and bus** floors and ceilings on a 0–4 scale,
gated by `Build.MANUFACTURER == "samsung"`.

This is a sanctioned API asking the platform to hold clocks, not a benchmark
whitelist being tricked. It is a far better lead than the Ludashi package-name
trick, and it reaches **bus** frequency — memory bandwidth — which nothing
else here touches and which a tiler is sensitive to.

**Unverified:** whether the SDK honours these requests for an unregistered
app. The code treats failure as normal — `initialize()` is wrapped in
try/catch and sets `isSamsungSdkAvailable = false` — which is consistent with
the calls being ignorable. That is precisely what the soak-ladder A/B would
show.

### FPS host-side: a Stage 10 assumption was too strong

Frame timing is captured by `GLRenderer.onFrameRenderedListener` →
`FrameRating.record()` → `FrameTimeRing.record()`, and a real community config
carries `"sessionMetadata":{"avg_fps":57.69,"session_length_sec":123}`.

So presented-FPS **is** observable outside the x86 process — because in a
Winlator-family runtime the **Android app is the presenter**. Wine renders
into the X server and the Android side composites to the display, so every
present passes through Java the runtime already owns.

The Stage 10 doc says FPS "exists only inside the rendering process". That is
wrong as stated; it is right only for *this* project's tooling, because the
profiler runs in **Termux**, a different app, and cannot hook another app's
renderer. The limitation is process isolation, not architecture.

`SystemMetricsReader.kt` also confirms the profiler's sysfs choices — same
`/sys/class/kgsl/kgsl-3d0/` paths, with a fallback ladder over `gpubusy`,
`gpu_busy_percentage` and `devfreq/gpu_load` worth borrowing.

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
- **A measured gain from VKD3D-Proton 3.0b** (installable on official Winlator
  today) over whatever the container currently defaults to. Upstream 3.0.1's
  mobile-GPU work — deferred clears, render pass suspend/resume — is a
  tiler-specific reason, and 3.0b is the nearest version reachable without
  changing forks.
- **A measured sustained-clock gain from GameNative's Samsung power control.**
  This is now the highest-value experiment available, because it targets the
  one factor already measured to be limiting. It needs no game and no Game
  Pass copy — soak ladder, profiler, `thermal` gate, compare `sustained_ms`.
- **A measured sustained-clock gain** from the Ludashi package name on
  Samsung, via the A/B above.
- A **measured** Box64 regression in 0.4.4 versus 0.4.0.
- A specific FH6 defect fixed in a newer VKD3D-Proton.
- Wine gaining a Vulkan 1.4 instance — though on inspection the 1.3.301 cap
  costs nothing here, since every capability VKD3D needs is present as an
  extension rather than requiring 1.4 core.
