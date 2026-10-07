# ps1-vj-mix — Usage

This is the **VJ mixer**. To get pictures on screen you also need at
least one **pcsx-redux fork** running a PS1 game and pushing its
primitive stream over IPC. The pcsx-redux fork lives at:

  https://github.com/pbfmp5g55-gif/pcsx-redux — grab the latest
  `v0.7.x` Windows release ZIP. **Mixer v0.10.x wants fork v0.7.10
  or newer** — that pairing is what the last real session
  (2026-05-15) was tested on. Older forks degrade in specific ways:
  v0.7.0 leaves `hostTag=0` so Clean CLUT goes black, and anything
  before v0.7.10 has a smaller IPC ring that drops frames on
  sprite-heavy scenes. **v0.7.11** also tells the mixer where each frame
  sits in VRAM and the display size (Controls shows `src=fork`); with an
  older fork the mixer guesses (`src=guess`): PAL titles that put their
  second buffer at y=256 land 16 lines low and wide modes (368/384) are
  cut at 320.

Both repos are Windows-only at the moment.

## What you need before starting

Three things, only one of which is shipped in the ZIP:

1. **The two builds** (mixer + pcsx-redux fork) — provided as release
   ZIPs above.
2. **A PS1 BIOS image.** The pcsx-redux fork ships *without* a BIOS;
   you need to drop one next to `pcsx-redux.exe`. See the
   **OpenBIOS** section below.
3. **A game disc image** (`.cue` + `.bin`, or `.iso`). Bring your own
   dump, or grab a homebrew like PSXFunkin
   (https://github.com/IgorSou3000/PSXFunkin) which is freely
   redistributable.

### OpenBIOS

The fork looks for `openbios.bin` (the open-source PS1 BIOS reimplementation
shipped with pcsx-redux). Drop the file next to `pcsx-redux.exe` and pass
`-bios openbios.bin` on the command line.

Download (`524288` bytes, SHA512 in a `.sha512` companion file):

- **Working mirror (MIT):**
  `https://mirrors.mit.edu/libreboot/canoeboot/old_releases/20250107/roms/playstation/openbios.bin`
- *Libreboot mirrorservice was 404 last we checked — use the MIT URL.*
- Alternative: build it yourself from the
  [pcsx-redux source tree](https://github.com/grumpycoders/pcsx-redux/tree/main/src/mips/openbios).

Other PS1 BIOS images (real-hardware dumps, e.g. `scph1001.bin`) work
too if you already own one — but redistribution of those is not
licensed, so we don't link them.

## What's in the box

After unzipping `ps1-vj-mix-windows-x64.zip`:

| File                       | Purpose                                  |
|----------------------------|------------------------------------------|
| `vj-mix-spike1.exe`        | The mixer GUI. Run this.                 |
| `vj-mix-ipc-selftest.exe`  | Smoke test for the shared-memory ring.   |
| `README.md`                | Project overview.                        |
| `design/*.md`              | Architecture / IPC / Spike plans.        |

## Quickest start (any Windows PC)

Unzip `ps1-vj-mix-windows-x64.zip` anywhere and drag one or two `.cue`
files onto `start-vj.bat` (two = channels A and B). The first run downloads
the emulator (fork v0.7.11, ~40 MB) into `pcsx-redux-A` and copies it to
`pcsx-redux-B` for a second game. `openbios.bin` (PCSX-Redux OpenBIOS, MIT,
see `OPENBIOS_LICENSE.txt`) is in the zip; games are not. Security software
may stop the freshly downloaded `pcsx-redux.exe` with a "new program"
prompt on first run; the script retries 5 times and then says so.

## Quick start (1 emulator + mixer)

1. **Run pcsx-redux** (`pcsx-redux.exe` from the v0.7.x fork ZIP).
   Use `-bios openbios.bin -iso <game.cue> -run` to start a game directly.
2. In pcsx-redux: **Debug → GPU → "Show VJ live IPC"**. Leave the
   ring name as the default `Local\vj-mix-prim-A`. Click **Start live**.
3. **Run `vj-mix-spike1.exe`**. The Controls window opens.
4. In the mixer Controls window:
   - Channel A name field already contains `Local\vj-mix-prim-A`.
   - Click **Attach** next to channel A.
   - Within a second you should see "ATTACHED, N frames received" growing.
5. The PS1 game's polygons now render inside the mixer window in real time.

   Shortcut for steps 1-4: launch the emulator with
   `-vjring Local\vj-mix-prim-A` (auto-starts Live IPC at boot) and the
   mixer with `--attach-a Local\vj-mix-prim-A` (auto-attaches at
   startup). A two-line `.bat` then brings the whole rig up.
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

## Show setup: projector + laptop

The mixer opens two OS windows:

- **Output** — the picture and the CROWD gauge, nothing else. Goes on the
  projector.
- **Controls** — every menu and effect setting. Stays on the laptop.

Before the show, set Windows to **Extend** (Win+P). In *Duplicate* mode
Windows reports one screen and there is nowhere to separate the two.

At boot the Output goes borderless-fullscreen on the first screen that is
not the Windows primary, shows `OUTPUT n` for 3 seconds, and Controls opens
on a screen that is not the Output. If the venue made the projector the
primary, pick the right screen in **Controls > Output** (the picture moves
at once) or start with `--output-monitor N`.

| Key | What it does (works with either window in front) |
|---|---|
| F11 | Output: fullscreen on the selected screen <-> normal window |
| F12 | BLACK. Output goes fully black; Controls shows a red bar |
| F1  | Hide / show Controls |
| F2  | Bring Controls back to a screen that is not the Output |
| Space (hold) | FLICKER: lets double-buffered games blink at their frame rate, for a hit. Same as the Controls button or a MIDI pad on the `FLICKER (hold)` CC (default 28, >= 64 while held). Cuts out after 3 s held; let go to re-arm |

Command line: `--windowed` (no automatic fullscreen), `--output-monitor N`
(1-based, as listed in Controls and the log; a number that does not exist
stays windowed), `--black` (start black).

If the projector cable is pulled, the Output drops back to a normal window
on the primary screen. Keys only work while one of the two mixer windows is
in front; for an emergency at the venue, also know where the switcher's own
black is.

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
- **CLUT 4/8bpp sprites look wrong / missing.** Pick the right
  **CLUT mode** in the mixer Controls panel:
  - *Direct sample* — legacy "VJ" mode, CLUT prims look broken on purpose.
  - *Discard CLUT* — silhouette only.
  - *Noise CLUT* — replaces CLUT sprites with per-prim hash colours.
  - *Clean CLUT (VRAM palette lookup)* — decodes the palette out of
    VRAM and shows the game's real graphics. **Requires
    pcsx-redux fork v0.7.1+** (the fork has to send TPage + CLUT in
    `hostTag`); with v0.7.0 you'll get a black screen in Clean mode.
  - *Shape only (vertex color)* — silhouette filled with the
    primitive's own vertex colour.
  - *Clean CLUT (inline palette)* — the fork ships the palette inline
    with each primitive (stream format v3), so this survives VRAM
    relocation. **Requires fork v0.7.6+.** This is the one to use in
    Phase C.
- **Clean mode flickers.** Live IPC ring drops under heavy traffic.
  Make sure you're on **pcsx-redux fork v0.7.10** (the ring grew
  2 MB -> 8 MB -> 32 MB across v0.7.2 .. v0.7.10; sprite-heavy
  frames need the 32 MB one). The per-channel `dropped=` counter in
  the mixer Controls window tells you if this is what's happening.

## Selftest

`vj-mix-ipc-selftest.exe` runs the shared-memory ring through three
scenarios (basic round-trip, wrap-around, backpressure drop) and prints
ALL OK. Use it to check whether ring problems are mixer-side or
emulator-side.
