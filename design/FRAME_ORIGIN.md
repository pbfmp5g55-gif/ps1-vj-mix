# FRAME_ORIGIN — where does each frame live?

Status: 2026-10-07. Implemented: fork pcsx-redux `vj-frame-meta` (FMD1, 52
bytes), mixer `feature/frame-origin`. The placement rule below is an
**empirical rule that worked on 3 titles**, not GPU semantics. The
lower-buffer guess (`liftLowerBuffer`, ebe7dbe) remains the fallback.

## Measured (Python ring reader, independent of the mixer)

Share of vertices inside the display window once the candidate origin is
subtracted:

| candidate | Nekketsu | Jet Ace | PSXFunkin |
|---|---|---|---|
| raw VRAM | 51.7 | 49.5 | 50.8 |
| drawing area start (E3) | **99.9** | **98.0** | **100** |
| drawing offset (E5, per prim) | 51.7 | 98.0 | 100 |
| display start, same VSync | 1.6 | 1.0 | 0.8 |
| display start, previous VSync | 99.9 | 98.0 | 100 |

Nekketsu keeps the offset at 0,0 and adds 240 itself. Jet Ace is PAL,
368 wide, second buffer at y=256 (the y=240 guess put it 16 lines low).
The display start at a VSync points at the *other* buffer. No title
changed the offset within a frame. quakepsx sends no primitives at all
(software renderer writing VRAM) and cannot be judged. Also found: the
software GPU never updates `gpu.m_display` (only OpenGL_GPU does), so the
fork reads the raw GP1 words instead.

Rule: origin = drawing area start, snapped to the previous VSync's display
start when within 32 px and that FrameEnd is the previous VSync of the same
stream. View extent = display size per channel (4:3 presentation).

## Completion review (GPT, session psvj_frame_origin_review)

Taken: per-channel view size (A/B with different sizes), snap only on
consecutive frames of one stream generation, GP1 cache cleared on GP1(00)
plus "seen since reset" bits, E3/E4 writes per frame counted and shown,
untested modes (24-bit, interlaced, display off, area moved mid-frame)
flagged in the readout, hres range check.

Not taken: transforming at draw time instead of at commit. Twin Self draws
delayed frames whose buffers alternate; transforming at commit keeps each
history frame at its own origin without a parallel origin ring. FLICKER is
"frames committed while held stay raw", which is what it does.

Open (not verified): savestate load (the GP1 cache is not restored; the
seen bits stay set from before), 480i / 24-bit FMV, titles beyond the 3,
the horizontal display range (GP1 06) is cached but the width uses the
nominal GP1(08) resolution.

## Problem (measured)

The fork sends vertices in VRAM space (`p.x + p.offset.x`). Double-buffered
games alternate the drawing offset between two buffers. Nekketsu Oyako,
read straight off the ring (90 frames): frames strictly alternate
y 0..273 / 240..513, x 0..398, ~240-256 prims each, none straddling.
The mixer maps x/320, y/240 onto the view, so every other frame was off
screen -> 30 Hz black flicker. Also x up to 398 > 320: the right side is cut.

The heuristic (>=90% of prims with minY >= 232 -> y -= 240) fixed this game,
but PAL titles at y=256 land 16 lines low, horizontal double buffering is
not handled, and the view size is still fixed at 320x240.

## What GPT's design review changed (session psvj_frame_origin)

- The first draft subtracted the **drawing area** start (GP0 E3). Wrong
  concept: the drawing area is a clip rectangle, not a transform. The
  transform is the **drawing offset** (E5); what the TV shows is the
  **display start** (GP1 05). All three are independent; a game with area
  (16,16) and offset (0,0) would be shifted 16 px.
- Two different products: "show what the TV shows" (subtract display start,
  phase matters) vs "show this frame's draw calls" (subtract each
  primitive's own offset; breaks for games that use the offset to centre
  geometry). Decide by measurement, not by argument.
- Display size is the VRAM source extent, not the presentation aspect.
  Keep 4:3 presentation. Out-of-range values = no metadata, never clamp.
- .vjr recordings would keep the old behaviour. Accepted for now: live
  only, recordings stay on the heuristic. Said so in the readout.
- Partial frames on ring overflow: already handled by the fork (whole
  frames are dropped, flushPendingFrame) and the reader resync (e0bdb05).

## Steps

1. **Fork sends everything, changes nothing** (pcsx-redux branch
   `vj-frame-meta`, 813e586). FrameEnd payload 'FMD1', 48 bytes, first 4
   bytes still the frame index:

   | off | type | field |
   |---|---|---|
   | 0  | u32 | frameIndex |
   | 4  | u32 | magic 'FMD1' 0x31444D46 |
   | 8  | u16 | size (48) |
   | 10 | u16 | flags: 1 display enabled, 2 interlace, 4 PAL, 8 24-bit, 16 offset seen |
   | 12 | i16 x4 | offset of the first / last primitive this frame |
   | 20 | u16 | offset changes within the frame (saturating) |
   | 22 | u16 | primitives this frame (saturating) |
   | 24 | i16 x2 | GP0 E5 offset at VSync |
   | 28 | u16 x4 | drawing area E3/E4 (10-bit fields) |
   | 36 | u16 x4 | display start x,y, display size w,h |
   | 44 | u32 | stream generation (new per live::start) |
   | 48 | u16 | E3/E4 writes during the frame (v52) |
   | 50 | u16 | GP1 seen since reset: 1 start, 2 v-range, 4 mode (v52) |

   Readers take the size field as the length (48 = first cut, 52 now).

2. **Measure** on the 4 local games (Nekketsu, Jet Ace, quakepsx,
   PSXFunkin) with a Python ring reader independent of the mixer: for each
   candidate origin (raw, area start, E5 at VSync, first-primitive offset,
   display start of the same / next / previous frame) the share of vertices
   that land inside the display window, and how often the offset changes
   within a frame.
3. **Mixer** uses the winning candidate when FMD1 is present and sane,
   view extent = display size (presentation stays 4:3), FLICKER keeps
   working (skip the transform), readout `src=fmd|guess`. Old fork -> the
   heuristic. Old mixer + new fork -> reads 4 bytes, unchanged.

## Verification

- Success: Nekketsu with the new fork shows no flicker with src=fmd and
  lifted=0, right edge visible; the other three games no worse than today.
  Evidence: ring dump scores + mixer readout + the user's eyes.
- Compatibility both ways: v0.7.10 fork with the new mixer; new fork with
  the main mixer build (328ad27).
- Not covered: games beyond the 4 local ISOs, 480i, 24-bit, render-to-
  texture-heavy titles. GPT suggested a synthetic test ROM for these; not
  in this pass.
