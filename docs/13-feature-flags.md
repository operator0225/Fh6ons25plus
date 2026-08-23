# Stage 13 — Feature flag contract

Defined now, before any optimisation exists, because it is a constraint on
how every later stage is written rather than an optimisation itself.

## Rules

1. **Every experimental optimisation is a flag.** No exceptions.
2. **Default is off.** A flag defaults on only after benchmarks on the actual
   S25+ show a repeatable win with no correctness regression.
3. **Independently togglable.** No flag may require another to be set.
4. **Fallback is the plain Vulkan path.** If a flagged path fails to
   initialise, log once and fall back — never crash, never silently continue
   in a broken state.
5. **A flag that does not measurably help gets deleted**, not left at 0.
   Dead flags are how a codebase accumulates untested paths.

## Registry

Flags are `STAR_BIONIC_*` environment variables. `0`/unset = off, `1` = on.

| Flag | Stage | Default | Status |
|---|---|---|---|
| `STAR_BIONIC_SHADER_CACHE` | 5 | 0 | not implemented |
| `STAR_BIONIC_ASYNC_PIPELINE` | 5 | 0 | not implemented |
| `STAR_BIONIC_FAST_DESCRIPTOR` | 3 | 0 | not implemented |
| `STAR_BIONIC_FAST_SYNC` | 3 | 0 | not implemented |
| `STAR_BIONIC_MEMORY_POOL` | 3 | 0 | not implemented |
| `STAR_BIONIC_CPU_AFFINITY` | 6 | 0 | not implemented |
| `STAR_BIONIC_DYNAMIC_RES` | 9 | 0 | not implemented |
| `STAR_BIONIC_PROFILER` | 10 | 0 | not implemented |

Every row is `not implemented`. The table exists so that the contract is
fixed before the code is, and so no stage can quietly introduce an
always-on experimental path.

## Non-flag switches

These select behaviour rather than enabling experiments, so they are not
subject to the "default off" rule:

| Variable | Values | Default |
|---|---|---|
| `STAR_BIONIC_GAME_SOURCE` | `steam`, `xodus` | `steam` |
| `STAR_BIONIC_LOG` | `error`, `warn`, `info`, `debug` | `warn` |

`xodus` is experimental for reasons unrelated to graphics — see
[`01-gamepass-msstore.md`](01-gamepass-msstore.md).

## What "measurably helps" has to mean

Per Stage 10/11, a flag is promoted to default-on only against, at minimum:
average FPS, 1% low, 0.1% low, and frame time — captured on the S25+, across
repeated runs, at a fixed resolution and preset.

Average FPS alone is not sufficient evidence. Several of the optimisations
listed above (async pipeline compilation especially) trade average
throughput for tail latency, which is precisely what 1% and 0.1% lows
measure and what an average hides.
