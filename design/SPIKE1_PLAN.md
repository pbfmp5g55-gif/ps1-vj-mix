# Spike 1 — `.vjr` replay via the mixer

The first executable artifact for this repo. Goal: load a `.vjr` file
(captured by the upstream `ps1-primitive-vj` recorder in v0.6.0) and play
it back as moving graphics in a window.

This validates the whole rendering pipeline before any IPC complexity.

## Milestones

### M1. Skeleton + window

- CMake project, single executable `vj-mix-spike1`.
- Link libvj as a subdirectory `add_subdirectory(third_party/libvj)`
  (matches upstream layout; submodule pin in this repo).
- GLFW + glad + imgui pulled via FetchContent (same versions the
  upstream pcsx-redux fork uses to make later widget reuse painless).
- Empty window + ImGui demo overlay. End state: `./vj-mix-spike1` opens
  a window with "Hello mixer" rendered.

### M2. Read a `.vjr` file

- `PrimitiveStreamReader` opens the file, prints frame count, total
  primitives, average per frame to stdout.
- ImGui side panel: file path text input + Open button + frame
  navigator (scrubber).

### M3. Untextured polygon rendering

- For each frame in the file: collect every `Primitive` with
  `textured == false`, build a triangle list (Quad → 2 triangles), draw
  via GL with vertex colour interpolation.
- Single fragment shader: `gl_FragColor = v_color;`
- Coordinate space: PS1 (0..320, 0..240) maps to GL clip space
  (-1..1, 1..-1) so y is flipped per PS1 convention.
- ImGui shows: current frame index, fps, primitives drawn.

End state: the file plays back as filled coloured polygons, without
textures. PSXFunkin's UI elements (which are sprites = textured) will be
invisible at this stage; the stage background polygons should be drawn.

### M4. Textured polygon rendering

- Extend the recorder (next `ps1-primitive-vj` minor) to write VRAM
  uploads alongside primitives. Bump `vj::PrimitiveStream` file format
  to v2, gated by a new record type byte.
- In the spike: maintain a 1024×512 RGBA8 texture; apply VRAM upload
  records as they appear in the stream. Textured primitives sample from
  it using `(u, v)` + the TPage offset extracted from `hostTag`.
- Two-shader pipeline: one for untextured, one for textured.

End state: PSXFunkin replays with arrows and characters visible.

### M5. Semi-transparency, dithering, mask bits

- Add a per-primitive blend mode bit to `Primitive::hostTag` if not
  already there (it carries TPage which has the blend semantics on PS1,
  so this is just extraction code).
- GL blend states per primitive: `glBlendFunc` per drawcall.
- Dithering via shader fragment ordered-dithering matrix.

This step is optional for "first working replay" — defer if time-bound.

### M6. Twin Self

- Take Spike 1 M3+ and add a `PrimitiveRingbuffer` (from libvj's
  `vj/PrimitiveStream.h` — already shipped in `ps1-primitive-vj` main).
- Live mode (input = a `.vjr` file streamed at original rate): push
  current frame's primitives into the ringbuffer, render them, then
  also render `ringbuffer.getDelayed(N)` for `N = delay slider`.
- Two drawcalls per frame; both go through the same renderer.
- Window: VJ slider for `delay (0..5s)`.

This is the first VJ-usable artifact from the new repo: a player that
shows live and 3-seconds-ago overlaid.

## Out of scope for Spike 1

- IPC (Spike 2).
- Two-source mixing (Phase B).
- VRAM relocation (Phase C).
- MIDI bindings (port from upstream later — same widgets work here).

## Estimate

- M1: 1 day
- M2: 0.5 day
- M3: 1 day
- M4: 2–3 days
- M5: 1–2 days (optional)
- M6: 1 day

Total without M5: ~5 days. With M5: ~7 days.

This is the unlock for both Phase A's deferred features (replay + Twin
Self in upstream become trivial frontends to this) and Phase B (which
just swaps the file reader for an IPC ring reader).
