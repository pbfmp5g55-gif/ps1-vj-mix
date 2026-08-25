# ps1-vj-mix

**VJ mixer for two PS1 emulators.** Phase 7 of the
[ps1-primitive-vj](https://github.com/pbfmp5g55-gif/ps1-primitive-vj) project:
take two `pcsx-redux` fork processes running independent games, intercept
each one's GPU primitive stream over IPC, and mix them through libvj into
a single output stream. Drive the mix from the GUI or from MIDI CCs.

This repo is the **mixer / orchestrator** — the central process. The
patched pcsx-redux fork (which adds the IPC sender hook) lives in the
[fork's `vj-integration` branch](https://github.com/pbfmp5g55-gif/pcsx-redux/tree/vj-integration).

## Status

**Shipped and working on Windows.** Latest mixer release **v0.10.2**
(2026-05-15); it needs pcsx-redux fork **v0.7.10** or newer. Both Phase B
and Phase C run end-to-end with two live emulators.

| Phase | Goal | Status |
|---|---|---|
| Phase A | 1 emulator + live IPC + render in mixer | ✅ shipped |
| Phase B | 2 emulator processes + crossfader (VRAM shared, intentional glitches) | ✅ shipped |
| Phase C | VRAM relocation per source (clean co-existence), sliding back to B's chaos | ✅ shipped |

Development has been idle since **2026-05-15**. No open branches, no
open issues — it stopped at a clean point. See
[`HANDOVER_ja.md`](HANDOVER_ja.md) for where to pick it up (Japanese).

To actually run it, read [`USAGE.md`](USAGE.md).

## Architecture (1-paragraph version)

Two `pcsx-redux.exe` instances run as separate OS processes. Each is
patched to forward its primitive stream over a **named shared-memory ring**
(`Local\vj-mix-prim-A` / `-B`) to this mixer. The mixer fuses both streams
(per-frame, source-tagged) through the libvj `PrimitiveInterceptor`
pipeline — so all the filter / preset / AutoMode plumbing applies on top —
then rasterises the result through its own PS1-emulating GPU surface
(OpenGL + Dear ImGui).

## Why two processes (not one binary)

PCSX-Redux's `g_emulator` is a singleton. Replicating it in-process would
mean rewriting half the emulator. Two processes sidesteps that entirely —
each is the same well-tested binary the upstream project ships. The cost
is an IPC layer, which is bounded engineering rather than a refactor.

## What the mixer can do today

Mixing / rendering
- **Crossfade A↔B** (0 = only A, 1 = only B, 0.5 = both at half density)
- **B VRAM relocate X** slider 0–512 px — 0 is Phase B's texture collision
  chaos, 512 is Phase C's clean split. Anything between is a continuous
  "collision amount" knob.
- **6 CLUT modes** for 4/8bpp palette sprites: Direct sample (legacy, broken
  on purpose) / Discard / Noise / Clean (VRAM palette lookup) / Shape only
  (vertex colour) / Clean (inline palette)
- **PS1 ordered dithering** (4×4 Bayer), strength 0–1
- **PS1 semi-transparency**: 5-bucket draw pipeline covering the four ABR
  sub-modes

Glitch (libvj)
- 8 live params: **MASTER / CHANCE / GEOMETRY / TEXTURE / MISSING / COLOR /
  DEPTH / CHAOS**, applied per primitive before rasterisation
- **AutoMode** LFO (depth + rate) modulating those params hands-free
- **Filter preset bank**: per-slot region / area / textured-only filters,
  saved to and reloaded from a file, morphable from a MIDI knob

Sources
- **Live IPC** from one or two emulators, with per-channel frame /
  drop / heartbeat counters
- **`.vjr` recordings** — open, play/pause, 0.1–4× speed
- **Twin Self** (past-frame ghost overlay) — *file source only for now*

Audience (CROWD)
- The room taps a gauge from their phones; it rides on top of whatever the VJ
  has set, and a full gauge fires a burst. Server, phone page and all the feel
  tuning live in [`crowd-server/`](crowd-server/README.md) (Node, no npm
  dependencies); the mixer keeps strength / CUT / HOLD / participation window.
- A **keyboard test mode** drives the same gauge with no server and no phones,
  so the look can be judged first. Design: [`design/CROWD_CONTROL.md`](design/CROWD_CONTROL.md).

Control
- **MIDI in** (RtMidi): port picker, refresh, per-target **Learn**.
  Bound targets: Twin Self enable / delay / brightness, CLUT mode,
  Crossfade, B VRAM relocate.
  ⚠ The 8 glitch params are **GUI sliders only** — they have no CC binding yet.
- **CLI**: `--attach-a <ring-name>` / `--attach-b <ring-name>` auto-attach
  at startup, so a launcher script can bring the whole rig up.

## Repo layout

- `src/mixer/` — the central process: `main.cpp` (GUI + render + mix),
  `ipc/` (shared-memory ring + selftest), `crowd/` (loopback UDP link to the
  crowd server), `gl_loader.*`
- `crowd-server/` — the audience feature's Node side, with its own tests
- `start-crowd.bat` — brings the crowd server and the mixer up together
- `third_party/libvj/` — git submodule → `ps1-primitive-vj`
- `design/` — `ARCHITECTURE.md`, `IPC_PROTOCOL.md`, `GPU_PIPELINE.md`,
  `SPIKE1_PLAN.md`, `SPIKE2_PLAN.md`

Two binaries are built: `vj-mix-spike1` (the mixer GUI) and
`vj-mix-ipc-selftest` (shared-memory ring smoke test).

## Build

```
git clone --recurse-submodules https://github.com/pbfmp5g55-gif/ps1-vj-mix
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Windows-only at the moment. CI publishes `ps1-vj-mix-windows-x64.zip` to
GitHub Releases on tag push.

## License

Mixer code: MIT. Anything that includes patches against pcsx-redux
remains GPL v2 per upstream license.

## See also

- [ps1-primitive-vj](https://github.com/pbfmp5g55-gif/ps1-primitive-vj) — libvj (MIT)
- [pcsx-redux fork](https://github.com/pbfmp5g55-gif/pcsx-redux) — `vj-integration` branch (GPL v2)
