# PC Game Pass / Microsoft Store — feasibility

Date: 2026-08-23 (revised)

Your FH6 copy is PC Game Pass, and a Steam copy is not an option.

This document records what that means. The short version: **FH6 cannot
currently be launched from a Game Pass copy on any platform outside Windows,
including x86_64 Linux — the blocker is upstream and has no announced
timeline.** The graphics work in Stages 2–12 is unaffected and continues.

---

## Why the Microsoft Store cannot be added to the build

The Microsoft Store, the Xbox app, and Gaming Services are **UWP / MSIX**
applications. They depend on the Windows Runtime app model, the AppX
deployment service, a licence-protected install location (`WindowsApps`), and
Store licensing services.

Wine does not implement that app model. There is no Wine or Proton build in
which the Microsoft Store installs and launches a Game Pass title. This is
not a missing DLL or a config flag — the deployment stack was never
implemented.

Stacking the problem: this project runs Wine **under Box64, on ARM64
Android**. Adding an unimplemented x86 UWP runtime on top of an x86→ARM64
translation layer is not an incremental cost.

So the Store is out. The question is whether the *game* can be reached
another way.

---

## Xodus — current status

**Correction to the earlier revision of this document.** It described the
Xodus path as workable-but-unverified, and said whether FH6 launches after
extraction "has to be tested". That understated the blocker. The accurate
position:

> Xodus has a developer-facing CLI and **no end-to-end game launch
> capability**. Sign-in, package download, and licence acquisition work.
> **Games do not launch** — that is blocked on reimplementing Microsoft's
> **Xbox Gaming Runtime** for Wine, which is not finished. No public release
> timeline has been announced.

So this is not "untested". No GDK title currently launches through Xodus, for
anyone, on any platform.

What the project is, factually:

| | |
|---|---|
| Started | 10 August 2026, by Heroic Games Launcher devs + contributors |
| Language | Rust; GPL-3.0 across several repos |
| `xodus` | login, package download, licence acquisition |
| `xal-rs` | reimplementation of the Xbox Authentication Library login flow |
| forked `ntfs` | parses and decrypts MSIXVC containers |
| Maturity | early development, ~47 stars, developer CLI only |
| **Cannot** | **launch a game** |

### What this path needs before FH6 runs on the S25+

Each of these is a separate unknown, and they compound:

1. **Xodus finishes the Xbox Gaming Runtime reimplementation** for Wine.
   No timeline. This is the hard blocker and it is entirely upstream.
2. **That runtime works under Box64 on ARM64.** Xodus targets x86_64
   Linux/macOS. Nobody has demonstrated the GDK runtime path through an
   x86→ARM64 translation layer, because there is not yet a working path to
   translate.
3. **MSIXVC2 does not break the package path.** Microsoft's new container
   format has general availability planned for **October 2026**; Xodus does
   not support it. This path has a plausible expiry date arriving before its
   blocker is resolved.
4. **FH6's own licence check passes** in that environment — still genuinely
   untested, and only reachable after 1–3.

This is not a "hard but tractable" list. Item 1 alone is an open-ended
reverse-engineering effort by a two-week-old project.

### Re-checked 2026-08-25 against Xodus's own README

Status **unchanged**: still cannot launch a game. Its README says *"The
project can now login, download packages and obtain licenses for games"*, and
the FAQ that *"there is still a lot of work to support it from Wine standpoint
to provide necessary XBOX Services to games."* The stated timeline is
literally **`soontm`**.

Two refinements, both in the pessimistic direction:

- **Decryption is less finished than reported.** Press coverage says Xodus
  "cracked" MSIXVC decryption; the repo tracks on-demand `.exe` decryption as
  an **open issue (#50)**. Take the repo over the headline.
- **MSIXVC2 is an open issue (#53)**, not work in progress — against an
  October 2026 GA. The expiry risk in item 3 above is real and unmitigated.

One thing that is *not* a new problem: FH6 is a 2026 GDK/MSIXVC title, so it
is in scope in principle. The out-of-scope list is older UWP titles — Gears of
War 4 is named, and early Forza Horizon releases fall in that category. **FH6
itself does not.**

**A gap worth naming explicitly:** Xodus solving this on Linux would not
automatically solve it here. It ships **its own Wine and Proton forks**, built
for x86_64 Linux and macOS. Winlator would have to carry those Wine patches in
its own ARM64 Android build, under Box64 or FEXCore. That is a second porting
effort that nobody has started, downstream of a first one that is not
finished.

---

## Measured from the source, 2026-08-25

"No timeline" is not the same as "no way to tell how close it is". The
organisation has eight repositories, and the two that decide this are
readable. Measure them with:

```sh
./tools/xodus-progress.sh
```

### The design, and where it breaks

```
FH6.exe (GDK)
   │  calls the GDK API
   ▼
xgameruntime.dll     ← xodus-gaming/xgameruntime  (C, Wine side, 5.6k lines)
   │  ✗ NO IPC CLIENT — this link does not exist yet
   ▼
$XDG_RUNTIME_DIR/xodus.sock
   │
   ▼
xodus-service        ← xodus-gaming/xodus  (Rust, host side, 300 lines)
   │  ✗ protobuf path is `unimplemented!()`
   ▼
Microsoft (auth · licensing · collections)   ← this part works
```

### First measurement

| | |
|---|---|
| `E_NOTIMPL` in the DLL | **346** |
| `return S_OK` | **30** — and mostly per-module registration, not real work |
| `FIXME` | 411 |
| Unimplemented share | **92%** |
| IPC from Wine side to service | **absent** |
| Protocol operations defined | **5**, of which 3 are `UNKNOWN`/`PING`/`PONG` |
| Real operations | **one** — `MSA_TOKEN_REQUEST`/`RESPONSE` |

**The scaffold is genuine work.** Every GDK module has an IDL and a C file —
`xuser`, `xstore`, `xpackage`, `xgamesave`, `xsystem`, `xtaskqueue` and a dozen
more. Recovering that API shape is the part that needed reverse engineering,
and it is done. The DLL correctly implements the GDK's `QueryApiImpl` dispatch
pattern, so its export table is only 7 entries with everything else behind a
function-table query.

**The bodies are empty, and the halves do not meet.** 346 functions return
`E_NOTIMPL`; the C side contains no socket client, and the Rust side's
protobuf path is a one-line `unimplemented!()`. So neither end of the bridge
between them exists, and the protocol they would speak carries one real
operation.

### What "close" would look like

`E_NOTIMPL` near zero, IPC present on the C side, and the protocol carrying
XUser / XStore / XPackage operations instead of a ping and one token request.
Re-run the script rather than reading announcements.

### Why this project is not going to finish it

Not because it is secret or enormous — it is roughly 350 function bodies and
an IPC bridge. Because **each of those bodies is defined by what a particular
game expects at runtime**, and the only way to learn that is to run the game
and watch it fail. That loop needs a Game Pass account, an MSIXVC package, the
title itself, and a debugger on the machine that runs it.

This project has already paid for writing code it could not execute: a Vulkan
probe that reported llvmpipe because the device's loader could not be seen
from here, and a `Z:\sys\class\kgsl` path that verified under plain Wine and
returned nothing on the phone. Three hundred and fifty unverifiable function
bodies against an undocumented API is that same mistake at scale.

The honest position is the one already recorded: this is upstream work, it is
being done by people who can test it, and the graphics stack below it proceeds
regardless.

---

## What this means for the project

**It does not stop the graphics work**, and that is not a consolation prize —
it is the actual point.

Stages 2–12 — Vulkan capability measurement, Star Bionic, shader and
pipeline caching, CPU affinity, Android overhead, profiling, benchmarking —
depend on the **device and the translation stack**, not on which storefront
sold the game. Every one of them proceeds unchanged. None of that work is
wasted or blocked by the Game Pass situation.

What is blocked is exactly one thing: **launching FH6 specifically, from a
Game Pass copy.**

### Practical consequence

The stack can be built, measured, and optimised now, and validated against a
DX12 title that *does* launch under Wine/VKD3D-Proton. When the Game Pass
path becomes viable — or if the acquisition situation changes — FH6 drops
into a stack that is already tuned, rather than one that has not been started.

The alternative is to build nothing until an upstream project with no
timeline finishes an open-ended task. That is strictly worse.

---

## Effect on the design

`star-bionic-run` treats game acquisition as a pluggable **source**, because
the entire graphics stack below it is unaffected by the choice:

```
STAR_BIONIC_GAME_SOURCE=generic  # default: an existing Windows install
STAR_BIONIC_GAME_SOURCE=xodus    # BLOCKED UPSTREAM -- cannot launch games
```

`xodus` stays defined so the integration point exists, and is documented as
non-functional rather than experimental. Wiring it up is cheap once upstream
can launch a game; pretending it works now would mean building against an
interface nobody has demonstrated.

**No Microsoft Store, Xbox app, or Gaming Services component is added to the
build image.** They would not function, and shipping them would imply a
capability that does not exist.

---

## Sources

- [Xodus: Xbox PC and Game Pass to Linux (GamingOnLinux)](https://www.gamingonlinux.com/2026/08/xbox-pc-and-game-pass-coming-to-linux-with-the-xodus-project/)
- [Heroic devs embark on Xodus (Tom's Hardware)](https://www.tomshardware.com/software/linux/xbox-pc-and-game-pass-titles-are-coming-to-linux-through-xodus-heroic-launcher-devs-embark-on-new-open-source-reverse-engineering-project)
- [Xodus cracks Xbox authentication and package decryption](https://www.techtimes.com/articles/323943/20260811/xodus-cracks-xbox-authentication-game-package-decryption-bring-pc-game-pass-linux.htm)
- [Xbox Game Pass on Linux via Xodus (XDA)](https://www.xda-developers.com/xbox-game-pass-games-might-soon-work-on-linux-thanks-to-this-open-source-project/)
- [Forza Horizon 6 PC specs (Forza Support)](https://support.forza.net/hc/en-us/articles/50088215399827-Forza-Horizon-6-PC-Specs)
