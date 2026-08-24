# Stage 2b — Turnip capabilities (measured through Winlator)

Date: 2026-08-24
Capture: `measurements/turnip-mesa-26.1.0-devel.json`
Method: `vkprobe.exe` run **inside the Winlator container**, so it loaded
`vulkan-1.dll` → winevulkan → Turnip. Same path the game takes.

| | |
|---|---|
| Driver | **`MESA_TURNIP`** — `turnip Mesa driver`, Mesa **26.1.0-devel** |
| Device | Adreno (TM) 830, INTEGRATED_GPU |
| Device API | **1.4.346** |
| Instance API | 1.3.301 (Wine's loader — see below) |
| Conformance | **1.4.0.0** |
| Extensions | 168 |
| `pipelineCacheUUID` | `3259f1ebbf9f4d87cb1a306e5f06cb57` |

This is the driver the stack actually runs on. [Stage 2](02-adreno830-capabilities.md)
measured the Qualcomm blob, which is the hardware ceiling; this is what
VKD3D-Proton will see.

---

## Two earlier conclusions are now wrong

### 1. `VK_EXT_memory_budget` — Turnip HAS it

Stage 2 recorded that this device exposes no runtime GPU memory budget, and
that Stage 7 would have to infer memory pressure from the OS. That was true
of the Qualcomm driver. **Turnip exposes it**, and the number it returns is
the most important measurement so far:

```
heap0   size 8.14 GiB   device_local
        budget 2.80 GiB
        usage  0.00 GiB
```

**The usable budget is 2.80 GiB, not 8.14 GiB.** The heap is the addressable
size; the budget is what the driver says is actually available under current
system pressure — and the idle baseline already showed ~7.8 GB of 12 GB in
use before the game starts.

For a 1080p/High target on a Forza-generation title, that is tight. It is now
the single hardest measured constraint in the project, and it is a *dynamic*
one: the budget shrinks as Android takes memory back. Stage 7 and Stage 9
have to be designed against a moving 2.8 GiB, not a static 8 GiB.

The good news is that it is now **queryable at runtime**, so Star Bionic can
react to it rather than guess.

### 2. Stage 10's "impossible" metrics are available after all

[Stage 10](10-profiler.md) listed pipeline creation, frame queue latency and
GPU memory as unreachable, and proposed writing a Vulkan layer for them.
Turnip ships the extensions that make three of them direct queries:

| Extension | Gives us |
|---|---|
| `VK_EXT_pipeline_creation_feedback` | **pipeline creation time**, per pipeline |
| `VK_KHR_present_wait` + `VK_KHR_present_id` | **frame queue latency**, real present timing |
| `VK_KHR_calibrated_timestamps` (+ `VK_EXT_`) | GPU↔CPU clock correlation |
| `VK_EXT_memory_budget` | **GPU memory**, live |
| `VK_KHR_pipeline_executable_properties` | compiled shader introspection |

Combined with the already-confirmed `timestampComputeAndGraphics` at
52.0833 ns, a layer can now measure everything Stage 10 asked for. The plan
stands; the shopping list got much shorter.

---

## What Turnip gains over the Qualcomm blob

| Capability | Turnip | Qualcomm | Why it matters |
|---|---|---|---|
| Device API | **1.4.346** | 1.3.284 | newer feature set |
| `VK_EXT_memory_budget` | **YES** | no | live memory pressure |
| `subgroupSizeControl` | **YES** | no | wave-size control in compute |
| `computeFullSubgroups` | **YES** | no | |
| `VK_EXT_graphics_pipeline_library` | **YES** | — | Stage 5: linkable pipeline stages |
| `VK_EXT_pipeline_creation_feedback` | **YES** | — | Stage 5/10: compile cost |
| `VK_EXT_shader_module_identifier` | **YES** | — | Stage 5: cache keys |
| `VK_EXT_descriptor_buffer` | **YES** | — | modern descriptor path |
| `VK_KHR_present_wait` / `present_id` | **YES** | — | frame pacing |
| `accelerationStructure` | **YES** | unconfirmed | ray query is genuinely usable |
| Extensions | 168 | 165 | |

All the VKD3D-Proton essentials survive: `textureCompressionBC`,
`descriptorIndexing`, `runtimeDescriptorArray`, `timelineSemaphore`,
`bufferDeviceAddress`, `nullDescriptor`, `mutableDescriptorType`,
`synchronization2`, `dynamicRendering`, `shaderInt64`,
`shaderDemoteToHelperInvocation`, `pipelineCreationCacheControl`,
`VK_KHR_push_descriptor`. Every BC format is still supported, format by
format, along with all depth/stencil and HDR render target formats.

Tessellation, geometry shaders, `depthBounds`, and all three fragment shading
rate levels are present too.

---

## What Turnip loses — and one of them matters a lot

### Only one usable queue

```
family0   count=1   GRAPHICS + COMPUTE + TRANSFER + SPARSE_BINDING
family1   count=1   SPARSE_BINDING only
```

The Qualcomm driver exposed three families with three queues in the first.
**Turnip exposes a single universal queue** — family1 does sparse binding and
nothing else.

D3D12 has three queue types: direct, compute, and copy. Games use the copy
queue to stream textures while the direct queue renders, and the compute
queue for async compute. With one Vulkan queue, VKD3D-Proton has to serialize
all three onto it.

For an open-world streaming title, that is a plausible frame-time problem:
texture uploads cannot overlap with rendering, they interleave with it. This
is a **specific, testable hypothesis** for Stage 4, and a candidate Star
Bionic target — not a conclusion. It has to be measured before anything is
built around it.

### Smaller losses

| Feature | Turnip | Note |
|---|---|---|
| `sparseResidencyImage3D` | **false** | D3D12 tiled 3D resources unavailable |
| `shaderStorageImageMultisample` | false | |
| `shaderFloat64` | false | rarely used by games |
| `uniformAndStorageBuffer16BitAccess` | false | fp16 in UBOs |
| `shaderBufferFloat32AtomicAdd` | false | float atomics partially present |
| `quadOperationsInAllStages` | false | |
| `residencyStandard3DBlockShape` | false | matches the 3D sparse gap |
| memory types | 4 | Qualcomm exposed 9 |
| `maxUpdateAfterBindDescriptorsInAllPools` | 16.7M | vs 83.8M — still ample |

Still absent on both drivers: `VK_EXT_mesh_shader` and
`VK_KHR_ray_tracing_pipeline`. Mesh shaders remain the one capability that
could hard-block FH6 if the game requires them, and that is still unverified
against the actual game.

---

## Wine caps the instance at 1.3

```
instance_api_version   1.3.301      (winevulkan)
device apiVersion      1.4.346      (Turnip)
```

The device speaks Vulkan 1.4, but the loader inside the container creates a
1.3 instance. **Vulkan 1.4 core features are therefore not reachable through
Wine right now**, regardless of what the driver supports. Anything 1.4-only
has to come in as an extension, or wait for a newer winevulkan.

Worth re-checking whenever Winlator's Wine is updated.

## External memory is invisible inside Wine

```
VK_KHR_external_memory_fd        false
VK_KHR_external_semaphore_fd     false
VK_ANDROID_..._hardware_buffer   false
OPAQUE_FD / SYNC_FD              neither importable nor exportable
```

This is **not** a Turnip limitation — it is winevulkan not surfacing the
Linux FD-based extensions to the Windows side. It matters only if something
needs to share memory across the Wine boundary; the game itself does not.

---

## What this changes

1. **Memory is the headline constraint.** 2.80 GiB of live budget, dynamic,
   on a title that expects desktop VRAM. Stages 7 and 9 are now about
   fitting inside that, and the budget is queryable so we can measure it.
2. **The single-queue finding is the first concrete Star Bionic hypothesis.**
   Whether serialized copy/compute actually costs frames is a Stage 4
   measurement.
3. **Stage 10's layer got easier and more valuable.** Pipeline creation
   feedback, present timing, calibrated timestamps and memory budget are all
   direct queries now.
4. **Stage 5 gained real tools** — graphics pipeline library, creation
   feedback, and shader module identifiers change how the cache should be
   built.

No performance has been measured. This is what the driver exposes, not how
fast it runs.
