# PC Game Pass / Microsoft Store — feasibility

Date: 2026-08-23

You asked to add the Microsoft Store to the build, because your FH6 copy is
PC Game Pass.

**That specific approach does not work, and it is worth being blunt about it
early rather than after a lot of wasted effort.** There are two real paths
instead. Both are described below, and the launcher is being designed to
support either.

---

## Why "add the Microsoft Store to the build" cannot work

The Microsoft Store, the Xbox app, and Gaming Services are **UWP / MSIX**
applications. They depend on the Windows Runtime app model, the AppX
deployment service, a licence-protected install location (`WindowsApps`),
and Store licensing services.

Wine does not implement that app model. There is no Wine or Proton build in
which the Microsoft Store installs and launches a Game Pass title. This is
not a matter of a missing DLL or a config flag — the deployment stack it
needs was never implemented.

Stacking the problem: this project runs Wine **under Box64, on ARM64
Android**. Adding an unimplemented x86 UWP runtime on top of an x86→ARM64
translation layer is not an incremental cost.

So the Store itself is out. What is *not* out is the game.

---

## Path A — Xodus (extract the Game Pass copy) — experimental

As of **10 August 2026**, Xodus (from the Heroic Games Launcher developers)
is an open-source project that handles **Xbox identity authentication** and
**encrypted MSIXVC package decryption**. It sits on top of Wine/Proton rather
than replacing them: it does not run the Store, it bypasses it, extracts the
game, and hands a normal install to Wine.

It targets modern **GDK / MSIXVC** titles. FH6 (May 2026) is in that
generation, so it is in scope — older UWP-era titles explicitly are not.

Honest assessment:

- **It is ~2 weeks old at time of writing.** Treat it as a moving target.
- It is x86_64 Linux tooling. Practical use here means running Xodus **on a
  PC**, extracting FH6, then copying the extracted install to the phone —
  not running Xodus on-device.
- Microsoft is moving to **MSIXVC2**, with general availability planned for
  **October 2026**. Xodus does not support it yet. This path has a plausible
  expiry date.
- GDK titles commonly perform an entitlement/licence check at launch. Whether
  FH6 specifically runs after extraction, and whether it needs a live Xbox
  login each session, is **unverified**. It has to be tested, not assumed.

Because of that last point, Path A cannot be called "working" until someone
actually launches the extracted build. This repo will not claim otherwise.

## Path B — Steam copy — recommended

**FH6 released on Steam the same day as Game Pass (19 May 2026.)**

A Steam copy removes the MSIX container, the Xbox auth dependency, the
Gaming Services dependency, and the MSIXVC2 expiry risk in one step. It is
also the configuration that Winlator-family runtimes and VKD3D-Proton are
actually exercised against, so problems you hit will be problems other people
have already hit.

It costs money you have, in a sense, already spent — which is genuinely
annoying, and it is your call, not mine. But the engineering difference is
large: Path B starts at "does FH6 run well on Adreno 830", while Path A
starts at "does FH6 launch at all", with an unrelated and actively shifting
DRM problem in front of the graphics work this project is actually about.

---

## Recommendation

Do the graphics work against **Path B**, and keep **Path A** as a flagged,
experimental track.

The reason is sequencing, not preference. Stages 2–12 — the Vulkan capability
work, Star Bionic, shader/pipeline caching, CPU affinity, profiling,
benchmarking — are **identical regardless of which copy of the game you
launch**. None of that work is wasted by choosing Path B now, and all of it
is blocked behind an unrelated DRM problem if you choose Path A now.

If the Xodus path matures, switching to it later costs one launcher flag.

---

## Effect on the design

`star-bionic-run` will treat game acquisition as a pluggable **source**,
because the entire graphics stack below it is unaffected by the choice:

```
STAR_BIONIC_GAME_SOURCE=steam    # default; expects an existing install
STAR_BIONIC_GAME_SOURCE=xodus    # experimental; expects a pre-extracted
                                 # MSIXVC install produced on a PC
```

The `xodus` source will be gated, off by default, and documented as
experimental with the MSIXVC2 caveat — consistent with the Stage 13 rule that
experimental paths are never on by default.

**No Microsoft Store, Xbox app, or Gaming Services component will be added to
the build image.** They would not function, and shipping them would imply a
capability that does not exist.

---

## Sources

- [Xodus cracks Xbox authentication and package decryption](https://www.techtimes.com/articles/323943/20260811/xodus-cracks-xbox-authentication-game-package-decryption-bring-pc-game-pass-linux.htm)
- [Xbox PC and Game Pass games being worked on for Linux](https://videocardz.com/newz/xbox-pc-and-game-pass-games-are-being-worked-on-for-linux)
- [Modders porting Xbox and PC Game Pass to Linux](https://ixbt.games/en/news/2026/08/11/427638-moddery-trudiatsia-nad-perenosom-xbox-i-pc-game-pass-na-linux.html)
- [Forza Horizon 6 release timing (Forza Support)](https://support.forza.net/hc/en-us/articles/51673280116115-Forza-Horizon-6-Release-Timing)
- [Forza Horizon 6 PC specs (Forza Support)](https://support.forza.net/hc/en-us/articles/50088215399827-Forza-Horizon-6-PC-Specs)
