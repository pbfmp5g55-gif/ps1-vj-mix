# ps1-vj-mix — Architecture

## Goal

Run two PS1 games concurrently, intercept each game's GPU primitive stream,
and feed both streams (source-tagged) into the libvj
`PrimitiveInterceptor` pipeline so the filter / preset / glitch path from
v0.5.0+ applies on top of a 2-source crossfade.

## High-level layout

```
+------------------+        IPC         +-------------+       +----------+
|  pcsx-redux  A   | ─── primitives ──▶ |             |       |          |
|  (game A)        |   (source tag A)   |             |       |          |
|  patched binary  |                    |    mixer    |  GPU  |  output  |
+------------------+                    |  (this repo)| ────▶ |  window  |
                                        |             |       |          |
+------------------+        IPC         |  + libvj    |       |          |
|  pcsx-redux  B   | ─── primitives ──▶ |             |       |          |
|  (game B)        |   (source tag B)   |             |       |          |
|  patched binary  |                    +-------------+       +----------+
+------------------+
                                              ▲
                                              │ MIDI
                                          (crossfader)
```

Each emulator runs unmodified game code; the patches only add a primitive
sender (a thin hook in `vj_bridge.cc`'s SubmitFn callback that, in
addition to the local write-back, packs the primitive into the IPC
channel). The mixer is a single GPU-owning process; only it has an actual
PS1 GPU surface on screen.

## IPC: shared memory ring + control socket

Per-emulator IPC consists of two endpoints:

1. **Primitive ring** — shared-memory ring buffer (SPSC), writer = emulator,
   reader = mixer. One slot per primitive; backpressure handled by the
   writer dropping primitives if the mixer falls behind (preferable to
   stalling the emulator). Format = the existing `vj::PrimitiveStream`
   record layout, framed by VSync markers.

2. **Control socket** — unix-socket / named-pipe for low-volume signals:
   open / close handshake, frame-sync requests, source-tag negotiation,
   crossfader value pushes. Doesn't need ring buffer scale.

Two emulators × two endpoints = four IPC channels.

## Source-tagging and the libvj entrypoint

The mixer drains both rings each frame, tagging primitives with
`source ∈ {A, B}`, and submits them sequentially to a single
`PrimitiveInterceptor`. A small new `Params` axis — `crossfade ∈ [0, 1]` —
gates which source survives:

```
keep =  source == A  ?  rand() < (1 - crossfade)
                     :  rand() < crossfade
```

`crossfade = 0` → 100% source A, `= 1` → 100% source B, `= 0.5` → both
sides drop about half their primitives, producing a noisy mid-mix that
gradually settles to one side as the knob moves.

## Frame sync

Two emulators run on separate threads at independent VSync rates. Naive
behaviour: each emulator pushes primitives at its own pace and the mixer
draws whatever's in flight at its own VSync. This is fine for Phase B and
even has its own VJ aesthetic (frames slipping in/out of phase).

For Phase C (clean co-existence) the mixer drives a single output VSync,
the master, and signals each emulator to advance one frame via the
control socket. Emulators then run their VSync handler, push primitives,
ACK, and wait for the next tick. This is a soft serialisation — the
mixer's output frame N corresponds to emulator frames N_A and N_B that
both ran exactly once for it.

## VRAM strategy

### Phase B — VRAM shared (intentional glitches)

Both source streams write to the same TPage / CLUT regions. PS1 GPU
"texture page" selection just resolves to whoever wrote last — so game
A's character can wear game B's car texture, etc. The libvj filter / glitch
path runs on top of this, exaggerating the chaos. This is the VJ stage
of the project: a single MIDI knob does the crossfade, the rest is
emergent.

### Phase C — VRAM relocated per source

Each source claims a half of the 1024×512 VRAM. As primitives arrive,
their `hostTag` (which encodes TPage / CLUT slot) is rewritten so that
source A's references land in A's allocated VRAM region and source B's in
B's. Any GPU VRAM-to-VRAM transfer commands the emulator issues are
rewritten the same way. Result: both games' textures co-exist without
trampling each other, both render correctly into the same on-screen frame.

The TPage rewrite is `tpage → tpage + sourceOffset`. A 64×64 TPage block
in a 1024×512 VRAM gives us 16×8 = 128 TPage slots; splitting half / half
gives each source 64 slots, more than most games actively use.

### B ↔ C toggle

Phase C's VRAM-relocation is one boolean. With it off, the system
degenerates back to Phase B's chaotic shared VRAM. MIDI-bind the toggle
(or expose a CC value 0..127 for partial relocation) to get a smooth
"chaos amount" knob.

## GP0 packet path (the hard part)

Replaying a recorded `vj::Primitive` from the mixer back into a PS1 GPU
surface requires more than calling `m_gpu->write0(this)` — that
expects a templated `PCSX::GPU::Poly<sh,shape,t,b,m>&`. Since we can't
synthesise the right template specialisation at runtime, we go around it
and submit raw GP0 packets to the PS1 GPU emulator instead. GP0 is the
PS1's standard command FIFO; one polygon = a header word + N vertex words.
This pipeline lives in `mixer/gp0_writer.{h,cc}` and is reused by Phase A's
recorded-stream replay in the upstream repo once it's mature.

## Implementation order

1. **Spike 1** — Mixer-only: read a recorded `.vjr` file (Phase A format),
   submit to a PS1 GPU via GP0 packets, draw on screen. Validates the GP0
   path with zero IPC complexity.
2. **Spike 2** — One IPC channel: patch pcsx-redux fork to ship its
   primitive stream to the mixer via shared memory. Mixer draws it.
   Validates the IPC piece with one emulator, no mixing yet.
3. **Phase B** — Two channels + crossfader. VRAM shared. Mixer logs which
   source wins each primitive.
4. **Phase C** — VRAM relocation. TPage rewrite per source. B↔C toggle.
5. **Polish** — MIDI binding, sync modes, recording the mixed output.

Spike 1 is also the missing piece from Phase A — it unlocks the recorded
stream replay feature deferred from `ps1-primitive-vj` v0.6.0.
