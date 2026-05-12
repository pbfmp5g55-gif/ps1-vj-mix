# Spike 2 — 1 emulator IPC (live)

Spike 1 plays back a recorded `.vjr` file. Spike 2 swaps the file
input for a live IPC connection to a running pcsx-redux fork — same
mixer, same renderer, but you watch your game live with no record/
replay round trip. This is also the foundation for Phase B (two
emulators) — Spike 2 just gets one channel working first.

## Components

```
+------------------+  shared mem ring  +------------------+
|  pcsx-redux fork | ────────────────▶ |   mixer (this)   |
|  + IPC sender    |                   |   + IPC reader   |
+------------------+                   +------------------+
        │                                       │
        └─ control pipe (open / hello / bye) ─┘
```

Per emulator:

1. **Primitive ring** — shared memory, SPSC. Writer = emulator
   (VSync listener), reader = mixer (frame poll).
2. **Control channel** — named pipe / file lock for open/close
   handshake. Optional in Spike 2; we can hardcode the segment name
   and discover liveness via writer heartbeat.

## Shared memory layout

A single fixed-size segment (suggested 2 MB) holding:

```c
struct RingHeader {
    uint32_t magic;        // 'VJRG' = 0x47524A56
    uint32_t version;      // 1
    uint64_t writeOffset;  // bytes from data[0], wraps at dataSize
    uint64_t readOffset;   // bytes from data[0]
    uint32_t dataSize;     // total bytes of data[] available
    uint32_t writerAlive;  // heartbeat counter, bumped per frame
    // pad / reserved
};
struct RingSegment {
    RingHeader header;
    uint8_t    data[];
};
```

Record framing inside `data[]` mirrors `vj::PrimitiveStream` per-record
layout (the v2 typed records, prepended with a 4-byte length so the
reader can skip records it doesn't recognise without a parser).

```
record:
    uint32_t lengthBytes;   // total record size including this header
    uint8_t  type;          // 0=Primitive, 1=VRAMUpload, 2=FrameEnd
    payload[length - 5]     // type-specific
```

Frame boundary: a `FrameEnd` record with `frameIndex` payload. Writer
emits one per VSync, mixer draws everything between FrameEnds as one
frame.

## SPSC discipline

- Writer reads its own `writeOffset` from local state, never from the
  segment.
- Reader reads `writeOffset` from segment, reads up to it, advances
  segment's `readOffset` when records are consumed.
- Both wrap at `dataSize`. Writer never overwrites unread data: if
  `writeOffset + need > readOffset (mod dataSize)`, writer drops the
  record (with a one-shot log) and continues. Frame integrity is
  preserved by always-or-never dropping a whole frame's records
  together — easier to recover from in the mixer.

## Platform notes

### Windows (primary target)

- `CreateFileMappingW` with `INVALID_HANDLE_VALUE` for an anonymous
  paging-file-backed mapping, named `Local\\vj-mix-prim-A`.
- `MapViewOfFile` to get the pointer.
- Control: named pipe `\\.\\pipe\\vj-mix-ctrl-A`.

### POSIX (future)

- `shm_open("/vj-mix-prim-A")` + `ftruncate` + `mmap`.
- Control: unix socket at `$XDG_RUNTIME_DIR/vj-mix-ctrl-A`.

Spike 2 can ship Windows-only; the abstraction lives behind a small
`mixer/ipc/ipc_ring.{h,cc}` interface so POSIX can land later without
touching call sites.

## Implementation milestones

### S2.1 — `IpcRingWriter` / `IpcRingReader` (mixer-side selftest)

Standalone unit (`mixer/ipc/`) that:
- Allocates a ring in the same process.
- Writer thread pushes synthetic records (e.g. random primitives).
- Reader main thread pulls them, validates.
- Runs as `vj-mix-ipc-selftest` exe so we can measure throughput +
  catch wrap-around bugs without involving pcsx-redux.

### S2.2 — pcsx-redux fork: IPC sender

In `vj_bridge.cc`, add a `live::` namespace alongside `record::`:
- `start(channelName)` opens the ring as writer.
- `stop()` flushes + unmaps.
- SubmitFn callback writes each primitive to the ring (same point that
  feeds `g_recordBuffer`). VRAM uploads similarly. VSync emits a
  `FrameEnd` record.
- UI panel (`live_panel.{h,cc}`) toggles, shows ring fill %.

### S2.3 — mixer: live source mode

Replace (or alongside) the file reader: a "Live" tab that opens the
shared-memory ring and streams frames straight into the existing
renderer pipeline. Same drawing code; just a different source of
`vj::EchoFrame` per frame.

Twin Self continues to work — the ringbuffer on the mixer side that
backs M6 is independent of the IPC ring; M6 just pushes whatever frame
arrives.

### S2.4 — release wiring

Bundle the mixer + the pcsx-redux fork sender in a single ZIP for VJ
PC use. Start order: launch pcsx-redux first (it creates the ring,
waits for a reader), then `vj-mix-spike1 --live`.

## Estimate

- S2.1 selftest: 1 day
- S2.2 pcsx-redux sender: 1 day
- S2.3 mixer live mode: 0.5 day
- S2.4 packaging: 0.5 day

Total ~3 days. After that, Phase B (two channels + crossfader) is a
matter of duplicating the IPC reader and adding a `Params.crossfade`
gate in libvj.

## Out of scope

- POSIX implementation.
- Mid-frame backpressure (we drop whole frames on contention, not
  records).
- VRAM-to-VRAM copy hooks (Phase C concern).
- Audio sync (separate concern; if needed, comes from pcsx-redux
  directly, not via this ring).
