# Stage 10 — Profiler

`tools/profiler.py` samples the system at a fixed interval and writes CSV plus
a JSON summary. It is the measurement baseline every later stage is judged
against.

## What it measures, and what it cannot

Stage 10 asks for thirteen metrics. They do not all live in the same place,
and pretending otherwise would produce a profiler that quietly reports
nothing useful.

### System-side — implemented

Read from `procfs`/`sysfs` outside the container, so they work during any
workload and need no root:

| Metric | Source |
|---|---|
| CPU usage, total and per core | `/proc/stat` deltas |
| CPU frequency per core | `cpufreq/scaling_cur_freq` |
| GPU usage | KGSL `gpu_busy_percentage` |
| GPU frequency | KGSL `gpuclk` / `devfreq/cur_freq` |
| GPU temperature | KGSL `temp` |
| GPU throttling | KGSL `throttling` |
| Temperatures | `/sys/class/thermal/thermal_zone*` (GPU/CPU/battery/skin) |
| RAM and swap | `/proc/meminfo` |

### Application-side — not implemented

These exist only inside the rendering process. Nothing outside the container
can see them, so the profiler reports them as unavailable rather than
guessing:

| Metric | Where it actually lives |
|---|---|
| FPS, frametime | the game's present loop |
| Shader compilation | VKD3D-Proton / DXVK |
| Pipeline creation | VKD3D-Proton / DXVK |
| Frame queue latency | the swapchain |
| GPU memory | no `VK_EXT_memory_budget` on this driver (see Stage 2) |

**GPU memory — corrected.** Stage 2 concluded there was no runtime memory
budget query. That held for the Qualcomm driver; [Turnip exposes
`VK_EXT_memory_budget`](03-turnip-capabilities.md) and reports a live **2.80
GiB** budget against an 8.14 GiB heap. It is queryable, from inside the
container. System RAM is still tracked here, but the Vulkan budget is the
number that will actually bound the game.

### Getting FPS — the plan

Three routes, in increasing order of effort and fidelity:

1. **DXVK/VKD3D HUD** — Winlator's HUD Mode. Easiest, but on-screen only, so
   it must be read off the display and it perturbs what it measures.
2. **VKD3D-Proton logging** — its env-var-driven logging can emit pipeline and
   shader-compilation events, which covers three of the missing metrics.
3. **A Vulkan layer** — a `VK_LAYER` that wraps `vkQueuePresentKHR` and uses
   timestamp queries for true GPU frame time. Highest fidelity and the only
   route to frame queue latency.

**The layer is now a much better deal than when this was written.** The
[Turnip capture](03-turnip-capabilities.md) shows the container has
`VK_EXT_pipeline_creation_feedback` (pipeline creation cost, per pipeline),
`VK_KHR_present_wait` + `VK_KHR_present_id` (real present timing, hence frame
queue latency), `VK_KHR_calibrated_timestamps` (GPU↔CPU clock correlation)
and `VK_EXT_memory_budget`. Every metric this section called unreachable is a
direct query on that driver.

Route 3 is what Stage 2 was checking when it confirmed
`timestampComputeAndGraphics` and a `timestampPeriod` of 52.0833 ns — a 19.2
MHz counter, roughly 320,000 ticks per 60 Hz frame. Ample resolution.

That work is deliberately not started: it belongs with Stage 4's VKD3D-Proton
integration, and none of it can be validated until a DX12 title actually runs.

## Design rules

Carried over from `vkprobe`, for the same reason:

1. **Every source is probed at startup** and its availability printed before
   sampling begins. You know what you are getting before the run, not after.
2. **An unreadable metric is an empty CSV cell and `null` in JSON — never 0.**
   A fabricated zero in a thermal or load trace silently drags an average
   down, which is worse than a visible gap.
3. **Rows are flushed every sample.** Android kills backgrounded Termux
   processes; a run that dies at minute 9 of 10 still yields nine minutes.
4. **The first CPU sample is null.** `/proc/stat` is cumulative, so a
   percentage only exists between two reads. Reporting 0% for the first
   sample would be inventing an idle second that never happened.

## Use

```sh
# baseline, 2 minutes
./tools/profiler.py --duration 120 --out runs/baseline --label "idle"

# during a game -- hold a wakelock first, or Android suspends Termux
# as soon as it loses foreground
termux-wake-lock
./tools/profiler.py --duration 300 --out runs/fh6-1080p-high \
                    --label "fh6 1080p high" --quiet
termux-wake-unlock
```

Ctrl-C stops early and still writes both files.

Output: `PREFIX.csv` (one row per sample) and `PREFIX.json` (availability,
per-metric min/mean/p50/p95/max, and every raw sample).

## Reading the output

The availability block at the top of a run is the first thing to check. If
`kgsl` says `NOT FOUND`, there is no GPU data in that run at all and the CSV's
`gpu_*` columns will be empty throughout — the run is still valid for CPU and
thermal, but it is not a GPU measurement.

The summary's `p95` matters more than `mean` for thermals and GPU clock: a
mean hides the throttling excursions that decide whether a frame rate is
sustainable, which is precisely what Stage 6 and 7 are about.
