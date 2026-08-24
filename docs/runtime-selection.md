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

| Runtime | Wine | Box64 | Turnip | State |
|---|---|---|---|---|
| **Winlator 11.2 Beta** (official) | — | **v0.4.4** | — | current beta |
| **Winlator 11.0** (official) | 10.10 | v0.4.0 | **26.1.0-devel** | what we measure |
| Winlator 10.1 | — | v0.3.6 | 25.1.0 | superseded |
| Winlator Cmod v13.1.1 | 9.20 / Proton 10 | older | bundled | older base |
| GameHub / GameNative / WinHub | — | — | — | separate lineage |

Notes worth having:

- **No fork's release notes mention Adreno 8xx or Snapdragon 8 Elite at all.**
  What actually decides Gen 8 support is the Turnip version, and 26.1.0-devel
  is the newest published in any of them. There is no Adreno-830-specific
  build to switch to.
- The widely-linked `Stredohori/Winlator-CMOD` mirror was **archived in May
  2026**; Cmod releases come from coffincolors directly. Its published base is
  Wine 9.20 / Proton 10 era — **older than official 11.0's Wine 10.10**.
- Reliable release *dates* could not be established. Sources conflict badly
  and much of what is indexed is content-farm material. Version-to-component
  mappings from GitHub releases are used instead, and those are consistent.

## Recommendation

**Stay on official Winlator. Do not switch forks.**

The reasoning is evidence, not preference:

1. **The GPU side is already optimal.** Turnip 26.1.0-devel is the newest
   Turnip any fork ships, every VKD3D-Proton requirement is present, and BC
   textures are native. There is nothing a different fork improves here.
2. **Cmod's published base is older** — Wine 9.20 against 11.0's 10.10. Moving
   there is a downgrade on the axis that is actually unmeasured.
3. **Switching forks means rebuilding the container**, discarding a
   configuration this project has measured across ten rounds. That is a
   regression risk taken against a stack known to work.

### The one targeted upgrade

**11.0 → 11.2 Beta, for Box64 v0.4.4 (from v0.4.0).**

Box64 is the x86_64→ARM64 translation layer, and it is the single largest
unmeasured factor in this project: the GPU has demonstrated headroom, so CPU
translation is the plausible next bottleneck. A dynarec improvement is the
one change with a specific reason behind it.

It is a beta. Keep the 11.0 APK so it can be rolled back, and re-run
`vkbench` after upgrading — the numbers are directly comparable now that
`thermal` gates the starting state.

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

- A fork shipping a **newer Turnip** than 26.1.0-devel with Gen 8 fixes.
- A **measured** Box64 regression in 0.4.4 versus 0.4.0.
- A specific FH6 defect fixed in a newer VKD3D-Proton.
- Wine gaining a Vulkan 1.4 instance — though on inspection the 1.3.301 cap
  costs nothing here, since every capability VKD3D needs is present as an
  extension rather than requiring 1.4 core.
