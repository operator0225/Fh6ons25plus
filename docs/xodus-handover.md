# Xodus work order — for a Claude Code session on a PC

You are being handed this because the previous session could not do the work,
and the reason is specific: it ran in a cloud container with **no Windows, no
Xbox account, no Game Pass package, no game, and no way to execute a single
line of what it would have written.**

You presumably have a machine. That changes which tasks are possible, and it
is the only thing that changes. Everything below was read directly from the
source on 2026-08-25 — not from press coverage, which is wrong in both
directions about this project.

---

## 0. What Xodus is, and the one thing it cannot do

Xodus runs PC Game Pass titles outside Windows. It **can** sign in to Xbox,
download MSIXVC packages, and obtain licences. It **cannot launch a game**,
and that is the entire blocker.

Its own README's timeline for that: `soontm`.

### The architecture, and where it breaks

```
game.exe (GDK title)
   │  calls the GDK API
   ▼
xgameruntime.dll          xodus-gaming/xgameruntime   C, ~5.6k lines
   │  ✗ NO IPC CLIENT — this link does not exist
   ▼
$XDG_RUNTIME_DIR/xodus.sock
   │
   ▼
xodus-service             xodus-gaming/xodus          Rust, ~300 lines
   │  ✗ protobuf path is `unimplemented!()`
   ▼
Microsoft  (auth · licensing · collections)      ← this part works
```

**Both ends of the bridge between the two halves are missing.** That is the
single most important fact in this document.

### Measured state

Re-measure any time with `./tools/xodus-progress.sh` from this repo.

| | |
|---|---|
| `E_NOTIMPL` in the DLL | **346** |
| `return S_OK` | **30**, and mostly per-module registration |
| `FIXME` | 411 |
| Unimplemented share | **92%** |
| IPC client on the C side | **absent** |
| Protocol operations | **5**, of which `UNKNOWN`/`PING`/`PONG` are plumbing |
| Real operations | **one** — `MSA_TOKEN_REQUEST`/`RESPONSE` |

**Be fair about what is already done.** The scaffold is genuine reverse
engineering and it is finished: every GDK module has an IDL and a C file —
`xuser`, `xstore`, `xpackage`, `xgamesave`, `xsystem`, `xtaskqueue`,
`xnetworking`, `xgameui` and more. The DLL implements the GDK's
`QueryApiImpl(REFCLSID, REFIID, void**)` dispatch correctly, which is why its
export table is only seven entries — everything else is reached through
COM-style versioned interfaces (`IXUserImpl` … `IXUserImpl6`).

Recovering that API shape was the hard, unglamorous part. **Do not rewrite
it.** Your job is bodies, not architecture.

---

## 1. The insight that makes this tractable

346 functions is not an actionable instruction. But you do not have to guess
which ones matter, and **you must not.**

Every stub in the tree looks exactly like this:

```c
static HRESULT WINAPI x_user_device_XUserFindControllerForUserWithUiAsync(
        IXUserDeviceImpl2 *iface, XUserHandle user, XAsyncBlock *async )
{
    FIXME( "iface %p, user %p, async %p stub!\n", iface, user, async );
    return E_NOTIMPL;
}
```

and every module file declares `WINE_DEFAULT_DEBUG_CHANNEL(gdkc)`
(`main.c` uses `xgameruntime`).

**So the DLL is already fully instrumented.** Run a title against it with:

```sh
WINEDEBUG=+gdkc,+xgameruntime wine game.exe 2> trace.log
```

and the log is a **call-ordered, deduplicated worklist of exactly the
functions that game needs**, with the arguments it passed. 346 unknowns
collapse to the twenty or thirty that actually run before the first hard
failure.

**This is the loop. Everything else in this document serves it:**

```
run → read the first FIXME that mattered → implement it → run again
```

Do not implement a function that has not appeared in a trace. It is
unverifiable work against an undocumented API, and it is how this goes wrong.

---

## 2. Setup

Eight repos exist under `github.com/xodus-gaming`. Four matter:

| repo | language | what it is |
|---|---|---|
| `xgameruntime` | C | the replacement DLL — **your main workspace** |
| `xodus` | Rust | CLI + `xodus-service` (the socket daemon) |
| `wine` | C | their Wine fork; the DLL builds **in-tree** |
| `xgameruntime-docs` | — | notes on the real DLL's internals — **read first** |

`xgameruntime/Makefile.in` reads `MODULE = xgameruntime.dll`,
`IMPORTS = combase`. That is a Wine in-tree module makefile, so the DLL is
built as `dlls/xgameruntime/` inside the `xodus-gaming/wine` checkout, not
standalone.

> **Not verified by the previous session.** It had no Wine build environment,
> so it never built this. Treat the build steps as *shape*, not gospel —
> follow each repo's own README, and if reality differs, reality wins.
> Say so rather than forcing these instructions.

Rough order:

1. Clone `wine`, and place/symlink `xgameruntime` as `dlls/xgameruntime`.
2. Add the module to Wine's `configure.ac` if their fork has not already.
3. Build Wine (a 64-bit build is enough; GDK titles are x86_64).
4. Build and run `xodus-service` from the `xodus` repo — it binds
   `$XDG_RUNTIME_DIR/xodus.sock` with mode `0600`.
5. Use `xodus-cli` to sign in and fetch a package. It is a developer CLI and
   it is the part that works.

---

## 3. Workstream A — the IPC bridge (do this first; no game needed)

This is the highest-value task available and **it is fully testable without a
game**, because both ends are defined by each other rather than by Microsoft.

Current state:
- C side: no socket code at all. Confirmed — no `AF_UNIX`, no `sys/socket.h`,
  no mention of `xodus` anywhere in the `.c` files.
- Rust side: `crates/xodus-service/src/connection/proto.rs` is literally
  `unimplemented!("Protobuf path isnt implemented yet")`.

The wire format is already defined and is trivial — from
`crates/xodus-service/src/connection/mod.rs`:

```
[ u32 magic LE ][ u16 msg_type LE ][ u16 payload_len LE ][ payload ]

magic 0x58445358  → XML framing   (implemented: Ping, MsaTokenRequest)
magic 0x58445350  → protobuf      (unimplemented!)
```

Responses are sent with `msg_type + 1`, which is why the enum pairs
`MSA_TOKEN_REQUEST = 3` / `MSA_TOKEN_RESPONSE = 4`.

### Tasks

1. **Write the C-side client.** A small `ipc.c` in the DLL: connect to
   `$XDG_RUNTIME_DIR/xodus.sock`, frame a message, block for the reply, hand
   back a buffer. Wine's unix-call interface (`__wine_unix_call`) is the
   correct way to do socket I/O from a PE module — do not call libc sockets
   directly from the PE side.
2. **Prove it with `PING`.** `XodusMessageType::Ping` echoes its buffer back.
   That is a complete round trip with zero Microsoft involvement and it is
   your first real milestone.
3. **Implement `proto.rs`** to mirror the XML path.
4. **Extend `common.proto`.** It currently has five entries. A game needs at
   minimum XUser sign-in, XStore licence query, and XPackage installation
   path. Add message types as the traces demand them — not speculatively.

**Definition of done for A:** the DLL sends `PING`, the service answers
`PONG`, and a `MSA_TOKEN_REQUEST` issued from inside Wine returns a real
token. No game required to prove any of it.

---

## 4. Workstream B — trace-driven implementation (needs the game)

Only start once A can carry a token, because `XUser`/`XStore` are meaningless
without it.

1. Acquire and decrypt the title with `xodus-cli`.
2. Run it under the patched Wine with `WINEDEBUG=+gdkc,+xgameruntime`.
3. Find the **first** `stub!` line that precedes the failure.
4. Implement that one function. Most fall into three kinds:
   - **Local** — `XPackageGetInstallationPath`, `XSystemGetDeviceType`.
     Pure filesystem or constants. No network, no service. Easiest wins.
   - **Async** — anything `…Async`/`…Result` with an `XAsyncBlock`. These need
     `XTaskQueue` to actually work; check `xtaskqueue.h` and
     `xasyncprovider.idl` before writing one, because getting the completion
     model wrong will look like a hang, not an error.
   - **Service-backed** — `XUserAddAsync`, `XStoreQueryLicenseToken`. These go
     over the socket. Workstream A must land first.
5. Re-run. Repeat.

Record each iteration: which function, what the game did next. That log is the
actual deliverable — it is what tells anyone whether this is converging.

---

## 5. Rules

These are not style preferences. Two of them are here because this project
already paid for breaking them.

- **Never implement a function no trace has demanded.** The previous session
  shipped a Vulkan probe that reported the wrong driver, and a sysfs path that
  verified under plain Wine and returned nothing on the real device. Both were
  written without the ability to run them. You can run things. Use that.
- **Never fake a Microsoft-signed token.** The licence flow (documented in
  `docs/xbox/gameruntime.md`) is RS256-signed by Microsoft and *verified* by
  the game. The design is to proxy genuine tokens the user is entitled to.
  Forging is both impossible and not what this is.
- **Never claim a function works because it compiles.** It works when the game
  gets past it. That is the only test.
- **Never delete or rewrite an IDL to make a build pass.** The IDLs are the
  reverse-engineered ground truth. If one seems wrong, that is a finding worth
  reporting upstream, not a file to edit around.
- **Keep Wine conventions.** `FIXME`/`TRACE`/`WARN` as they are used now,
  existing naming, existing vtable layout. This is LGPL-2.1 Wine-style code
  intended to go upstream.

---

## 6. What "close" looks like

Report against these, not against effort spent:

| signal | now | done |
|---|---|---|
| `E_NOTIMPL` | 346 | near zero **for functions the game calls** |
| IPC client in the DLL | absent | present, `PING` round-trips |
| `proto.rs` | `unimplemented!()` | implemented |
| protocol operations | 1 real | XUser + XStore + XPackage |
| the game | does not launch | reaches a menu |

**"Reaches a menu" is the milestone that matters.** Everything before it is
scaffolding, and no percentage of 346 is evidence of anything on its own.

---

## 7. Upstream

`xgameruntime` is LGPL-2.1 (see the header in `main.c`); `xodus` is GPL-3.0.
This is interoperability work of the same kind as Wine itself, on titles the
user is entitled to through a paid subscription.

Contribute upstream rather than maintaining a private fork. The project is
active — `xodus` had commits on 2026-08-25. Check open issues first: MSIXVC2
support is #53 and on-demand `.exe` decryption is #50, so those are known and
owned.

---

## 8. What this does *not* solve

Even finished, this is **x86_64 Linux/macOS**. The parent project targets a
Galaxy S25+ — ARM64 Android under Winlator.

Running it there additionally requires those Wine patches carried into
Winlator's own ARM64 Wine build, under Box64 or FEXCore. **That is a second
port and nobody has started it.**

So: this work is worth doing, and it does not by itself put FH6 on the phone.
Do not report it as if it does. The graphics work in the parent repo proceeds
independently and is not blocked by any of this — see
[`01-gamepass-msstore.md`](01-gamepass-msstore.md).
