# Stages 4 / 5 / 7 — `vkbench` measurement harness

`tools/vkbench/` answers three questions that gate the next three stages.
None of them need FH6, or any game, running — which matters, because the
Game Pass copy [cannot currently launch](01-gamepass-msstore.md).

Run it inside the Winlator container and it measures the same driver
VKD3D-Proton will use.

---

## Why the harness comes before the optimisation

Stages 4, 5 and 7 are optimisation stages. The project's own rule is that
nothing gets called optimised without a before/after benchmark, so the
benchmark has to exist first. Building the shader cache before being able to
measure compile cost would mean shipping something whose value is an opinion.

Each test also has a specific hypothesis attached, drawn from the
[Turnip capture](03-turnip-capabilities.md) — so a result either confirms a
real problem worth attacking, or removes it from the list.

---

## Test 1 — queue topology (Stage 4)

**Hypothesis:** Turnip exposes a single universal queue, so D3D12's direct,
compute and copy queues all serialise onto it, and texture streaming cannot
overlap rendering.

Enumerates every family with its flags and `queueCount`, identifies whether a
dedicated compute or transfer family exists, and states the consequence in
terms of what was actually found:

```
family0  count=1  GRAPHICS+COMPUTE+TRANSFER+SPARSE_BINDING
graphics family queue count : 1
separate queues possible    : False
-> only one usable queue: D3D12 direct, compute and copy all serialise
   onto it, so uploads cannot overlap rendering
```

`queueCount` is the number that matters, not the family list. Two families
where the graphics family has one queue is still one usable queue for
rendering work.

## Test 2 — pipeline creation cost (Stage 5)

**Hypothesis:** a persistent pipeline cache meaningfully cuts first-run
stutter.

Compiles N compute pipelines that differ by specialization constant, so each
is a genuinely distinct compile rather than a deduplicated repeat. Three
passes:

| Pass | Cache | What it tells you |
|---|---|---|
| `cold` | fresh, empty | the real cost of compiling from scratch |
| `warm_same_cache` | same object, still in memory | best case, same process |
| `seeded_from_blob` | **new cache built from the serialised blob** | the case Stage 5 actually ships: a cache reloaded from disk on a later run |

`seeded_from_blob` is the one that counts. A cache that only helps within one
process does nothing for launch stutter.

Where `VK_EXT_pipeline_creation_feedback` is present, the driver's own
per-pipeline duration is recorded alongside wall time, plus how many
creations reported a cache hit.

**Read `cache_hits` before believing `warm_speedup_x`.** Validating against
lavapipe, which does not implement pipeline caching at all, wall time still
dropped 1.35x across passes — pure warm-up — while `cache_hits` stayed 0 and
the cache blob was 32 bytes of header. Wall time alone would have reported a
35% win from a cache that did nothing.

## Test 3 — memory budget under pressure (Stage 7)

**Hypothesis:** the 2.80 GiB budget Turnip reports is real and dynamic, and
is the binding constraint on this device.

Allocates device-local memory in chunks until the driver refuses or a cap is
reached, re-querying `VK_EXT_memory_budget` as it goes, then frees
everything.

The key output is `usage_tracked_allocations`: **did `heapUsage` actually
move when we allocated?** If it did not, the budget is decorative and Stage 7
cannot lean on it. On lavapipe it does not — 512 MB allocated, usage
unchanged — which is exactly the failure this check exists to catch.

Defaults are conservative (128 MB chunks, 4 GB cap) and everything is freed
before exit. `--skip-memory` opts out.

---

## Test 4 — what the single queue costs (Stage 4)

Turns the confirmed single-queue topology into a number. Three measurements
on the same queue: compute dispatches alone (`render_only`), buffer copies
alone (`upload_only`), and both interleaved. If interleaved equals the sum,
nothing overlapped and uploads cost their full time on top of rendering.

GPU time from timestamp queries is preferred over wall time, since CPU submit
overhead is not what the test is about.

**Each mode is submitted twice and only the second is measured.** The modes
run in sequence, so without a warm-up pass the first absorbs cache misses,
clock ramp and first-touch faults while later ones look artificially fast.
The lavapipe validation had `interleaved` beating `render_only` while doing
strictly more work — impossible, and how the confound was caught.

Synthetic proxy, not FH6. It bounds the effect; it does not predict the
game's frame time.

## Test 5 — GPU frame-time curve (Stages 8/10)

Sweeps GPU load across five levels and reports measured GPU time per frame
plus implied FPS, so the load at which this device drops under a 60 Hz budget
can be read directly.

**Offscreen and synthetic.** No swapchain, no vsync, no compositor, and
compute dispatches are not a game frame. This measures the timestamp path and
the GPU's raw throughput curve — not presented FPS. The swapchain path stays
unwritten because it cannot be validated without a display, and shipping
unvalidated code to the device has gone badly before.

The first iteration at each level is discarded as warm-up.

## Use

```sh
# Termux -- copy the exe somewhere shared
cd ~/Fh6ons25plus && git pull
cp tools/vkbench/prebuilt/vkbench.exe ~/storage/downloads/
```

Then in the Winlator container, launch `vkbench.exe`. No arguments needed —
it writes `vkbench-result.json` **beside the exe**, for the same reason
`vkprobe.exe` does: Winlator launches by tap, and the working directory it
hands the process is neither chosen nor visible.

```sh
# back in Termux
cd ~/Fh6ons25plus
./tools/bench-report.py /sdcard/Download/vkbench-result.json
```

Options, if launched with a shell: `--pipelines N`, `--chunk-mb N`,
`--cap-mb N`, `--skip-memory`, `--out FILE`, `--lib PATH`, `--device N`.

## Validation status

Built clean under `-Wall -Wextra`, native and mingw. Exercised end to end
against lavapipe: all three tests run, JSON is valid, the exe writes beside
itself from an unrelated working directory, and both negative findings above
(`cache_hits=0`, `usage_tracked_allocations=false`) were detected correctly.

**It has not been run on Adreno 830.** The numbers above are lavapipe's and
are quoted only to show the checks work.
