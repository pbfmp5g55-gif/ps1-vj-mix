# ps1-vj-mix — Usage

This is the **VJ mixer**. To get pictures on screen you also need at
least one **pcsx-redux fork** running a PS1 game and pushing its
primitive stream over IPC. The pcsx-redux fork lives at:

  https://github.com/pbfmp5g55-gif/pcsx-redux — grab the `v0.7.0`
  Windows release ZIP.

Both repos are Windows-only at the moment.

## What's in the box

After unzipping `ps1-vj-mix-windows-x64.zip`:

| File                       | Purpose                                  |
|----------------------------|------------------------------------------|
| `vj-mix-spike1.exe`        | The mixer GUI. Run this.                 |
| `vj-mix-ipc-selftest.exe`  | Smoke test for the shared-memory ring.   |
| `README.md`                | Project overview.                        |
| `design/*.md`              | Architecture / IPC / Spike plans.        |

## Quick start (1 emulator + mixer)

1. **Run pcsx-redux** (`pcsx-redux.main.exe` from the v0.7.0 fork ZIP).
   Use `-bios <bios> -iso <game.bin/cue> -run` to start a game directly.
2. In pcsx-redux: **Debug → GPU → "Show VJ live IPC"**. Leave the
   ring name as the default `Local\vj-mix-prim-A`. Click **Start live**.
3. **Run `vj-mix-spike1.exe`**. The Controls window opens.
4. In the mixer Controls window:
   - Channel A name field already contains `Local\vj-mix-prim-A`.
   - Click **Attach** next to channel A.
   - Within a second you should see "ATTACHED, N frames received" growing.
5. The PS1 game's polygons now render inside the mixer window in real time.
6. Optional:
   - Enable **glitch effects** at the bottom and push **MASTER / CHANCE /
     GEOMETRY / TEXTURE / MISSING / COLOR / DEPTH / CHAOS** for live
     VJ-style distortion of the mixed stream.
   - **Twin Self** (delay overlay) only works in file mode for now —
     `Open` a `.vjr` recording instead of attaching live to use it.

## Phase B (2 games, crossfade)

1. Run **two** instances of `pcsx-redux.main.exe`, each with its own
   game. They need different memcard / config dirs if you start both at
   once (set `--persistent_dir <path>` differently for each).
2. In emulator 1: ring name = `Local\vj-mix-prim-A`, Start live.
3. In emulator 2: ring name = `Local\vj-mix-prim-B`, Start live.
4. In the mixer: Attach to channel A, then to channel B.
5. Move the **Crossfade A<->B** slider:
   - 0.0 → only game A is drawn
   - 0.5 → both games drawn at ~half density each (overlapping chaos)
   - 1.0 → only game B is drawn

## Phase C (clean co-existence, VRAM split)

Phase B's overlap normally collides in VRAM (both games' textures fight
for the same 1024×512 region). To get a clean two-game mix:

- Push the **B VRAM relocate X** slider to **512** (or hit the
  **Phase C (clean)** button). Channel B's textures and primitives now
  live in the right half of VRAM; channel A keeps the left half.
- Slide the value between 0 and 512 for a continuous "collision amount"
  knob — partway sometimes looks better than either extreme.

## Recorded files (.vjr)

You can also point pcsx-redux at the file recorder
("Show VJ recorder" → Start), produce a `.vjr` file, and replay it
in the mixer via the **Open** button in the file section. Useful for:
- Iterating on the mixer without re-running the emulator.
- Sharing captures with someone else.
- Using Twin Self (delay overlay), which currently only runs on file
  source.

## Troubleshooting

- **Attach fails immediately.** The named segment doesn't exist yet.
  Did you click Start live in pcsx-redux first? Names must match exactly
  (`Local\vj-mix-prim-A` etc).
- **Mixer window is dark / no polygons.** Channel attached but no
  `FrameEnd` records received — check that the emulator isn't paused.
  Press the **run** button in pcsx-redux or start it with `-run`.
- **Frame stats grow but nothing draws.** Untextured + textured polygons
  may both be empty if the game hasn't issued any geometry yet (intro
  splash). Wait a few seconds.
- **Garbled textures with both channels.** Push the VRAM relocate slider
  to 512 (Phase C clean mode) — Phase B chaos is the default.
- **CLUT 4/8bpp sprites look wrong / missing.** Known limitation — the
  current renderer samples VRAM as 15bpp direct-colour. M5 / future
  milestones plan CLUT support.

## Selftest

`vj-mix-ipc-selftest.exe` runs the shared-memory ring through three
scenarios (basic round-trip, wrap-around, backpressure drop) and prints
ALL OK. Use it to check whether ring problems are mixer-side or
emulator-side.
