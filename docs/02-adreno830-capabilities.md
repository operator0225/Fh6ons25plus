# Stage 2 — Adreno 830 measured capabilities

Date: 2026-08-24
Source: `vkprobe` on the device, via `/system/lib64/libvulkan.so`

| | |
|---|---|
| Device | Galaxy S25+ (SM-S936N), Android 16 |
| SoC | SM8750 (Snapdragon 8 Elite), platform `sun` |
| GPU | `Adreno830v2` (from KGSL `gpu_model`), max clock 1.2 GHz |
| Vulkan device | `Adreno (TM) 830` |
| Driver | Qualcomm proprietary, build `f61dec9117`, **dated 2026-03-26** |
| API | **1.3.284** |
| Conformance | 1.3.8.0 |
| Device extensions | 165 |

**This is the Qualcomm proprietary driver, not Turnip.** See "What this does
not tell us" at the end — it matters for planning.

---

## Headline: capability is not the blocker

Every hard requirement a D3D12 translation layer leans on is present. This is
a desktop-class Vulkan 1.3 feature set on a phone.

| Capability | Result | Why it matters |
|---|---|---|
| `textureCompressionBC` | **YES** | FH6 ships BCn textures. No transcode, no decompression, no VRAM blowup. |
| BC1/3/4/5/6H/7 (+sRGB) | **all YES** | Confirmed format-by-format, not inferred from the feature bit. |
| `descriptorIndexing` | YES | D3D12 descriptor heap model |
| `runtimeDescriptorArray` | YES | |
| `descriptorBindingPartiallyBound` | YES | |
| `timelineSemaphore` | YES | `ID3D12Fence` maps directly |
| `bufferDeviceAddress` | YES | D3D12 root descriptors |
| `nullDescriptor` (robustness2) | YES | D3D12 null-descriptor semantics |
| `mutableDescriptorType` | YES | descriptor heap emulation |
| `synchronization2` | YES | modern VKD3D-Proton barrier path |
| `dynamicRendering` | YES | avoids render-pass object churn |
| `shaderInt64` | YES | |
| `shaderDemoteToHelperInvocation` | YES | HLSL `discard` semantics |
| `pipelineCreationCacheControl` | YES | **Stage 5 async compilation** |
| `VK_KHR_push_descriptor` | YES | |

The BC result was the single largest open risk in this project. It is
resolved, and resolved well.

### Also present, and useful

- **`tessellationShader` and `geometryShader`** — both YES. Not a given on
  mobile; geometry shaders in particular are commonly absent. FH6's terrain
  and foliage paths are unlikely to hit a wall here.
- **`depthBounds`** — YES. Also frequently missing on mobile.
- **`attachmentFragmentShadingRate`** — YES. Variable rate shading is a real
  performance lever for Stage 8/9, and a better one than dropping resolution.
- **All depth/stencil formats** — `D24_UNORM_S8_UINT`, `D32_SFLOAT_S8_UINT`,
  `S8_UINT`, plus `R16G16B16A16_SFLOAT`, `B10G11R11_UFLOAT`,
  `A2B10G10R10_UNORM` for HDR render targets.
- **Sparse** — `sparseBinding`, `sparseResidencyBuffer`,
  `sparseResidencyImage2D` all YES. D3D12 reserved/tiled resources work.
- **`timestampComputeAndGraphics`** YES, `timestampPeriod` **52.0833 ns**
  (a 19.2 MHz counter). Stage 10 can measure real GPU frame time via
  timestamp queries — a 16.67 ms frame is ~320,000 ticks, ample resolution.
- **fp16 and int8** — `shaderFloat16` and `shaderInt8` both YES.

---

## Gaps that shape the plan

### 1. No mesh shaders
`mesh_shader __source: not-available` — `VK_EXT_mesh_shader` is absent
entirely, so the block was not queried (not "queried and false").

If FH6 has a mandatory mesh-shader path, this is a hard stop. Most 2026
titles still ship a conventional vertex path, but that is an assumption about
this game and **must be verified against the actual build**, not assumed here.

### 2. No ray tracing pipeline
`ray_tracing __pipeline_source: not-available` — no
`VK_KHR_ray_tracing_pipeline`, so DXR 1.0 raytracing pipelines cannot be
translated.

Curiously, **`VK_KHR_ray_query` is present**. That is the DXR 1.1 inline
raytracing shape. Whether it is actually usable depends on
`VK_KHR_acceleration_structure` and its `accelerationStructure` feature,
which this report does not surface — see "Open items". Either way, RT
features in FH6 should be off for now.

### 3. No `VK_EXT_memory_budget`
`memory_budget __source: not-available`. There is **no runtime query for GPU
memory pressure.**

This directly affects Stage 7. Memory headroom has to be inferred from
`/proc/meminfo` and KGSL sysfs instead of asked for, and any Star Bionic
memory pooling cannot rely on a budget signal to back off. Worth designing
around early rather than discovering later.

### 4. Fixed subgroup size 64
`subgroupSize 64`, `min/max 64 / 64`. No variance, no `subgroupSizeControl`
range to exploit.

This is actually convenient: wave64 matches AMD GCN/RDNA conventions that
VKD3D-Proton and DXVK shader paths already handle well. Shader work can
assume wave64 unconditionally.

---

## Memory

```
heap0   10.85 GiB   device_local=true
heap1    4.00 GiB   device_local=true
9 memory types
```

**Do not read these as 14.85 GiB of usable VRAM.** This is a unified-memory
SoC; the heaps are very likely overlapping views of the same physical RAM
with different access properties, not additive pools. The actual budget is
bounded by system RAM minus everything else Android is doing — and with no
`VK_EXT_memory_budget`, that has to be measured from the OS side.

For a game whose install is ~150 GB and which expects desktop VRAM, memory
behaviour is a live risk that Stage 7 has to measure, not assume.

---

## Selected limits

| Limit | Value | Note |
|---|---|---|
| `maxImageDimension2D` | 16384 | fine |
| `maxBoundDescriptorSets` | **7** | tighter than desktop's usual 8; VKD3D's set layout must fit |
| `maxPerStageResources` | 50331648 | effectively unbounded |
| `maxUpdateAfterBindDescriptorsInAllPools` | 83886088 | ample for descriptor indexing |
| `maxPushConstantsSize` | 256 | standard |
| `maxComputeWorkGroupInvocations` | 1024 | standard |
| `maxColorAttachments` | 8 | fine |
| `timestampPeriod` | 52.0833 ns | 19.2 MHz counter |

`maxBoundDescriptorSets = 7` is the one worth watching. It is one below the
desktop norm and VKD3D-Proton's set allocation should be checked against it.

## Queues

Three families: counts 3, 1, 1. Enough distinct families to map D3D12's
direct / compute / copy queues, **if** the flags line up — which this report
does not yet show. See "Open items".

## Cache key material (Stage 5)

```
pipelineCacheUUID   c9de610f435100000000010005440000
driver build        f61dec9117 / 2026-03-26
API                 1.3.284
```

All three belong in the Stage 5 cache key, alongside VKD3D and game version.
A driver update changes `pipelineCacheUUID` and must invalidate the cache.

---

## What this does *not* tell us

**These are the Qualcomm proprietary driver's numbers.** Winlator-family
runtimes typically run **Turnip** (Mesa) via AdrenoTools instead, and Turnip's
Adreno Gen 8 support is new — Mesa 26.0 landed initial Gen 8, with A830
configuration arriving around 26.1.0.

Turnip will very likely expose a **narrower** feature set than what is
measured above. So this document establishes the **hardware ceiling as exposed
by the mature vendor driver**, not what the stack will actually get.

A standalone Termux binary cannot reach an AdrenoTools-injected driver — it
lives inside the Winlator app process. Measuring Turnip requires running the
probe inside that container, and is tracked as an open item.

---

## Open items

1. **Turnip capture** — re-run the probe inside a Winlator container to get
   Turnip's actual feature set. Every gap between it and this document is a
   Star Bionic work item.
2. **Queue family flags** — which family is graphics / compute / transfer.
   Present in the JSON, not yet surfaced by `report.sh`.
3. **`accelerationStructure` feature** — resolves whether `VK_KHR_ray_query`
   is genuinely usable.
4. **FH6 mesh-shader dependency** — does the game require a mesh path?
5. **Real memory budget** — measure from the OS, since Vulkan will not say.

---

## What this means for the next stage

Capability is not the constraint. Nothing in this feature set blocks the DX12
path, and the one risk that could have sunk the approach — BC textures — came
back clean.

That relocates the performance question entirely. If 1080p/High/60 is out of
reach, it will be because of **CPU translation overhead (Box64), shader
compilation stutter, memory bandwidth, or thermal throttling** — not because
the GPU lacks a feature.

Which means Stage 3's target list changes. Star Bionic should not be hunting
for missing capabilities to emulate; it should be attacking CPU-side overhead,
pipeline/shader latency, and memory behaviour. Those are measurable now, and
they are where the frames will actually be won or lost.

**No performance claim is made here.** Nothing has been benchmarked. This
document describes what the hardware exposes, not how fast it runs.
