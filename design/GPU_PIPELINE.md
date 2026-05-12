# GPU pipeline — choosing how the mixer renders

The mixer takes `vj::Primitive` records (from a `.vjr` file in Spike 1, or
from IPC rings in Phase B) and must put them on screen. PS1 primitives
have specifics — semi-transparency modes, dithering, sub-pixel coords,
TPage / CLUT texture indirection — that need to be honoured for the
output to look "PS1".

Two paths considered:

## Option A — Self-contained OpenGL renderer

The mixer owns an OpenGL (or Vulkan) context, a VRAM-shaped texture
(1024×512 RGBA), and a small shader that emulates PS1 quirks.
`vj::Primitive` records map directly to triangle / quad lists, fed via
glDraw. Texture sampling indexes into the VRAM texture using the
`hostTag`-encoded TPage / CLUT offsets.

**Pros:**
- No dependency on pcsx-redux's GPU code; mixer is self-contained.
- Easy to add VJ-style post-processing in shader (scanlines, CRT bend,
  per-source tinting for debugging).
- Phase C VRAM relocation is purely a TPage-offset rewrite + texture
  region — clean inside this model.
- Cross-platform without dragging in the emulator's window / input layer.

**Cons:**
- Re-implementing PS1 GPU semantics from spec is its own project.
  Semi-transparency math, MaskBit, sub-pixel triangle rasterization rules,
  16bpp vs 24bpp framebuffer, CLUT 4-bit / 8-bit textures, line drawing
  edge cases — all need careful porting.
- Visual fidelity drift: it'll look "close to PS1" but not byte-identical
  to pcsx-redux's renderer. For a VJ aesthetic this is probably fine but
  worth naming up front.

## Option B — Reuse pcsx-redux's GPU as a library

Strip the GPU subsystem out of pcsx-redux and link it in. Feed it raw
GP0 packets (the same words the emulator sends after parsing the CPU's
DMA). pcsx-redux already supports software and hardware (GLSL) renderers
with switchable output paths.

**Pros:**
- Byte-identical visuals to single-source pcsx-redux output.
- Renderer code is battle-tested by the upstream project.

**Cons:**
- `g_emulator` singleton + tight coupling to the rest of pcsx-redux makes
  "GPU-only" extraction non-trivial. Likely needs upstream-friendly
  refactor or a heavy private fork.
- Linking a GPL v2 dependency forces the mixer into GPL v2 as well.
- Build complexity inherits pcsx-redux's (sentry, ffmpeg, lua,
  imgui-md, etc) — many transitive deps for a process that only needs
  the rasterizer.

## Decision: start with Option A

Spike 1 builds the self-contained OpenGL renderer, with deliberately
limited fidelity:
- Untextured polygons only at first (gouraud + flat shading).
- Add textured polygons with a single 32-bit VRAM texture next.
- Add semi-transparency mode 0 (B/2 + F/2) third.
- Dithering, MaskBit, line primitives, CLUT 4/8bpp tables later as
  needed.

Each step is a separable PR; we can stop at "good enough for visuals"
before chasing per-pixel fidelity.

Option B remains available as a future "high-fidelity" mode if the
project ever wants byte-identical PS1 output; the IPC protocol and
crossfade logic don't change.

## GP0 packet vs `vj::Primitive`

We do NOT need to round-trip via GP0 packets if we're building our own
renderer. `vj::Primitive` already contains everything the renderer needs
(vertices, UVs, vertex colours, host-tagged TPage / CLUT). GP0 packing is
only relevant for Option B.

This simplifies Phase A's "replay a .vjr file" significantly: read
PrimitiveStream → draw via OpenGL. No PS1 emulator state to reconstruct.

## Texture and VRAM

Spike 1 doesn't need real VRAM yet — untextured polygons use only
vertex colour. When textured primitives come in (Spike 1 step 2):

- Allocate a single OpenGL RGBA8 texture of 1024×512 (the same shape
  as PS1 VRAM).
- The IPC patch (Phase B) needs to forward GPU VRAM-to-VRAM uploads and
  CPU-to-VRAM uploads so this texture stays in sync. For Spike 1, the
  recorder records VRAM uploads inline in the `.vjr` stream as a new
  record type alongside primitives. `vj::PrimitiveStream` will need a
  small extension to carry these — file format version bumps to 2.

## Output window

GLFW + ImGui (same stack the upstream pcsx-redux uses for its window) is
the easiest path. ImGui gives us free UI for the crossfader / source
display / preset bank that all ports back from libvj's pcsx-redux side.
The mixer can reuse the existing widgets verbatim — they take
`vj_bridge` references that are equally valid here.

## Coordinate space

PS1 native resolution = 320×240 (NTSC, varies). The renderer's framebuffer
is a 320×240 (or similar) FBO; output window upscales with nearest /
bilinear / shader-CRT-bend depending on aesthetic. This keeps glitches
crisp and gives us a place to add post-processing later.
