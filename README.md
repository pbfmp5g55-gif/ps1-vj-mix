# ps1-vj-mix

**VJ mixer for two PS1 emulators.** Phase 7 of the
[ps1-primitive-vj](https://github.com/pbfmp5g55-gif/ps1-primitive-vj) project:
take two `pcsx-redux` fork processes running independent games, intercept
each one's GPU primitive stream over IPC, and mix them through libvj into
a single output stream. Drive the mix with a MIDI crossfader knob.

This repo is the **mixer / orchestrator** — the central process. The
patched pcsx-redux fork (which adds the IPC sender hook) lives in the
[main fork](https://github.com/pbfmp5g55-gif/pcsx-redux/tree/vj-integration).

## Status

Pre-implementation — design phase only. Phases below are planned, not
shipped.

| Phase | Goal | Status |
|---|---|---|
| Phase B | 2 emulator processes + IPC + crossfader (VRAM shared, intentional glitches) | Not started |
| Phase C | VRAM relocation per source (clean co-existence), with optional toggle back to B's chaos | Not started |

## Architecture (1-paragraph version)

Two `pcsx-redux.main` instances run as separate OS processes. Each is
patched to forward its primitive stream — via shared memory or a
unix-socket / named-pipe — to this mixer. The mixer fuses both streams
(per-frame, source-tagged) through the existing libvj
`PrimitiveInterceptor` pipeline (so all the filter / preset / MIDI plumbing
from v0.5.0+ still applies on top), then renders the result through a
single PS1-emulating GPU surface in this process.

## Why two processes (not one binary)

PCSX-Redux's `g_emulator` is a singleton. Replicating it in-process would
mean rewriting half the emulator. Two processes sidesteps that entirely —
each is the same well-tested binary the upstream project ships. The cost
is an IPC layer, which is bounded engineering rather than a refactor.

## What this repo will contain

- `mixer/` — the central process: connects to both emulator IPC endpoints,
  runs libvj merge, drives a PS1 GPU output surface
- `protocol/` — wire format for primitive streams + control messages,
  shared with the pcsx-redux patches
- `design/` — architecture docs, IPC protocol, GP0 packet pipeline
- `scripts/` — launchers, integration tests
- `examples/` — recorded `.vjr` files for replay-without-emulator demos

## License

TBD. Mixer code can be MIT; anything that includes patches against
pcsx-redux remains GPL v2 per upstream license.

## See also

- [ps1-primitive-vj](https://github.com/pbfmp5g55-gif/ps1-primitive-vj) — libvj (MIT)
- [pcsx-redux fork](https://github.com/pbfmp5g55-gif/pcsx-redux) — vj-integration branch (GPL v2)
