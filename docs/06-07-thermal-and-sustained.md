# Stages 6 / 7 — thermal behaviour, and what it constrains

Date: 2026-08-24
Everything here is measured on the S25+ through Turnip inside Winlator.
Raw data: [`measurements/RESULTS-2026-08-24.md`](../measurements/RESULTS-2026-08-24.md).

---

## The shape of this device

Adreno 830 has three distinct performance regimes, and which one a benchmark
lands in depends entirely on how long it runs.

| regime | clock | when |
|---|---|---|
| un-ramped | ~222–525 MHz | bursts under a few seconds — DVFS has not reacted |
| boost | **1200 MHz** | after ~1 s of load, holds ~15–20 s |
| thermal | **~600 MHz** | after ~77 °C is reached, indefinitely |

Sustained clock is **~50% of boost**. Full clock holds for about fifteen to
twenty seconds and then halves.

This is not a subtle effect and it invalidates any short benchmark as a
planning tool. It also runs in the *opposite* direction to intuition: a
four-second test does not measure boost, it measures a GPU that never woke
up. Measured at 65,536 workgroups, the eight-frame test read 15.86 ms while
the same load sustained 6.87 ms once the clock ramped — the short test was
**45% slower than the throttled steady state**.

## Sustained capability

Soak ladder, 45 s per level, run back to back from a verified-cold start
(38.0 °C, 222 MHz idle):

| load | peak | sustained | degradation | sustained FPS | 60 Hz |
|---|---|---|---|---|---|
| 16,384 | 1.506 ms | 3.232 ms | +114.6% | 309 | yes |
| 65,536 | 6.870 ms | 10.123 ms | +47.4% | 99 | yes |
| 131,072 | 17.318 ms | 19.631 ms | +13.4% | 51 | no |

```
sustained_groups_at_60hz = 110,639
```

**Degradation shrinks as load rises.** A lightly loaded GPU boosts hard and
has further to fall; a heavily loaded one is already near its thermal ceiling
and barely moves. So the headline "performance halves" applies to light
loads. At a load actually near the 60 Hz budget the drop is ~13%.

That is a more useful framing for a game: the closer the workload sits to the
device's sustained capability, the less the throttle hurts — because the
device was never going to boost far above it anyway.

## Design constraints this sets

1. **Target the sustained clock, not the boost.** Anything tuned against the
   first fifteen seconds will fall apart in minute three. Star Bionic has to
   work at 607 MHz and 50 °C.
2. **Benchmark from a known thermal state.** `star-bionic-run thermal` gates
   this. Two runs from different starting temperatures are not comparable,
   which cost four measurement rounds to work out.
3. **Run long enough to reach the regime you care about.** Under ~20 s
   measures un-ramped or boost, never sustained.
4. **Read memory headroom live, do not plan against a budget.** `heapBudget`
   is an estimate that *rises* as you allocate — five readings on this device
   spanned 2.66–4.88 GiB, and every run exceeded its own initial figure.
   `headroom = budget − usage` is the signal that actually falls.
5. **Treat sub-1 GiB headroom as the danger zone.** Past that Android kills
   the container rather than failing the allocation — measured at ~7 GiB
   allocated.

## Can FH6 do 1080p / High / 60 FPS?

**Not answerable from this data, and here is exactly why.**

What is measured is a synthetic compute workload. A game frame is vertex and
fragment work, render passes, texture sampling, and bandwidth — none of which
a compute dispatch stands in for. `sustained_groups_at_60hz = 110,639` is a
real number about this GPU; it is not convertible into FH6 frames.

What the data does establish:

**Favourable**
- BC textures are native — no transcode, which was the largest single risk.
- Every VKD3D-Proton requirement is present on Turnip.
- A persistent pipeline cache gives 55–103x on compile, with a deterministic
  244,765-byte blob, plus a separate Mesa shader cache worth ~8x on its own.
- Memory reaches ~7 GiB, far above the 2.8 GiB the first budget reading
  suggested.
- The single-queue limitation is real but partial — 0.20–0.51 overlap
  measured, not the full serialisation feared.

**Unfavourable**
- Sustained clock is half of boost, and a game runs for hours.
- Frame-time consistency collapses before throughput does: spread went from
  0.2% to 44% at the load where the GPU starts working hard.
- No mesh shaders, no ray tracing pipeline.
- Wine caps the instance at 1.3.301 while the device offers 1.4.346.

**Unmeasured, and each could decide it**
- FH6's actual GPU cost per frame at 1080p/High.
- Box64 CPU translation overhead — untested, and a plausible bottleneck given
  the GPU has headroom.
- Presented FPS through a real swapchain, with vsync and the compositor.
- Whether FH6 requires a mesh-shader path.

And the game cannot currently be launched at all from a Game Pass copy — see
[`01-gamepass-msstore.md`](01-gamepass-msstore.md).

**So the honest position: the GPU is not obviously the limiting factor, the
thermal behaviour is the main measured risk, and the CPU side is completely
unmeasured.** Anyone claiming a frame rate for this configuration right now
is guessing.

## Open

- Longer soak. The 65,536 level had not settled at 45 s (6.87 → 10.92 →
  9.82); 90–120 s per level would confirm the true steady state.
- CPU/Box64 overhead — needs a workload that actually exercises translation.
- Presented FPS — needs the swapchain path, which needs a display to
  validate against.
