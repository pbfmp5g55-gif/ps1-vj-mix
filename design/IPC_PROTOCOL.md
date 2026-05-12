# IPC Protocol

## Channels per emulator

### 1. Primitive ring (shared memory, SPSC)

- One slot per primitive. Layout matches `vj::PrimitiveStream` per-record
  format (kind / textured / vertex_count / hostTag / vertices) + one byte
  prepended for `source` tag.
- Frame boundary = a marker record with sentinel `vertex_count = 0xFF`
  (chosen because real PS1 primitives never exceed 4 vertices).
- Ring size sized for ~2 frames of worst-case prim throughput (~10K
  primitives / frame upper bound × 80 bytes / primitive ≈ 1.5 MB). Writer
  drops if full; mixer logs drop count.

### 2. Control socket (named pipe on Windows / unix socket elsewhere)

Bi-directional, length-prefixed JSON messages. Low frequency, no rt
constraints.

| Message | Direction | Purpose |
|---|---|---|
| `hello` | emu → mixer | initial handshake (source tag, capabilities) |
| `hello_ack` | mixer → emu | accept, assign final source tag |
| `tick` | mixer → emu | "advance one frame, push primitives, then ack" (Phase C sync mode) |
| `tick_ack` | emu → mixer | "frame N pushed" |
| `pause` / `resume` | mixer → emu | global pause control |
| `crossfade` | (none, mixer-local) | crossfader value isn't sent over IPC |
| `goodbye` | either way | clean disconnect |

## Endpoint names

Use deterministic names so the mixer doesn't need discovery:

```
shm:  vj-mix-prim-A   /   vj-mix-prim-B
ctrl: vj-mix-ctrl-A   /   vj-mix-ctrl-B
```

Both emulators can pass their own source tag (A or B) via command-line
flag at launch.

## Frame budget

PS1 frame ≈ 16.6 ms (60 Hz). The mixer has to:
1. Drain both rings
2. Run libvj interceptor on the merged stream
3. Submit GP0 packets to its own GPU surface

All within a single 16.6 ms window. The ring is read-only-mostly; the
hot path is the GP0 submission. Spike 1 will measure how many primitives
per frame is realistic before drops kick in.

## Backpressure and source desync

Two emulators can drift apart by tens of ms. Two strategies:

- **Phase B mode**: each emulator runs free; mixer draws whatever's in
  flight. Phase difference shows as visible asymmetry, which is
  intentional VJ noise.
- **Phase C mode**: mixer drives via `tick` messages, emulator pushes one
  frame, waits. Hard sync. Latency adds up to ~1 frame, fine for visuals.

Switching modes = a single flag in the mixer.
