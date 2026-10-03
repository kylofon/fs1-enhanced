# Engine rules — Flight Simulator 1 (from the faithful port v0.1.0)

The engine in `src/` is the faithful port's (`../Flight Simulator 1`, tag v0.1.0, commit fdb9bf1). This
file condenses its rules so that Enhanced sessions rarely need the faithful repo. Deep detail stays
there, searched and never dumped: `docs/subphases/3.*.md` (one per native area; code comments cite
them), `docs/PHASE3_PLAN.md`, `extracted/fs1.asm` (disassembly), `tools/fs1dis.py`.
Copied here: `docs/symbols.txt`, `docs/PROGRAM_MAP.md`, `docs/SCENERY_FORMAT.md`.

## Architecture
- `src/main.c`: SDL3 app callbacks, options, disk lookup, frame loop (one frame = 1/60 s of PC time),
  screen output (CGA 640×200 bitmap → composite or RGB colours → window).
- `src/sched.c`: the C scheduler and the boot in C. It calls the natives in the original's order and
  raises the timer (int8) and keyboard (int9) interrupts between them.
- `src/natives/*.c`: every original routine in C, one file per area (`docs/PROGRAM_MAP.md` "Frame
  structure"): `mainloop.c`, `flight_*.c`, `transform.c` (projection, clipping), `scenery.c`
  (scenery interpreter), `horizon.c`, `line.c`/`raster.h` (lines into the CGA bitmap), `panel.h`,
  `gauges.c`, `indicator.c`, `text.c`/`textdraw.h`, `keys.c`, `sound.c`, `editor.c`, `war.c`.
- `src/native.c`: the native table (name, original address); `--list-natives`.
- Emulator build only (`-DFS1_EMULATOR=ON`, `build-emu/`): `cpu8086.c` and the emulated PC in `pc.c`,
  which run the original code for `--verify` / `--native-off`.

## Memory model
- The 8088 memory image and register file (`cpu_core.c`) stay the natives' data model: game state lives
  at its original addresses (DS 0618 etc.).
- `src/game/state.h`: typed accessors generated from `docs/symbols.txt` by `python tools/gen_state.py`.
  Edit symbols.txt, regenerate; never hand-edit state.h. Search it, never read it whole.
- Numbers are the original's fixed-point formats (`PROGRAM_MAP.md` "Number formats", `fixmath.h`).

## Timing
- Fixed: 60 frames per second of emulated time; the sim, sound and keyboard run at the original rates.
- `*_cycles.h` tables charge the original's cycle counts so that the frame timing (and with it key
  latching and sound) is exact; they were generated in the faithful repo from the disassembly.
- F12 speeds the emulation up (×2/×4/×8, sound only at ×1).

## Video
- The natives draw into the CGA bitmap (B800) exactly as the original; `main.c` decodes it (composite
  artifact colour by default, RGB with F11 / `--rgb`) and scales it to the window.
- Natural hook points for a resolution-independent renderer: line drawing (`raster.h`), sky/ground fill
  (`horizon.c` `fill_rows`), text (`textdraw.h`), panel sprites and needles (`panel.h`, `gauges.c`),
  scenery dots, and the projection before rounding (`transform.c`).

## Input and sound
- Keys go through the original's int9 path as PC scan codes; `main.c` maps SDL keys (keypad stand-ins:
  arrows, Home/End/PgUp/PgDn, Ins/Del). `--keys`, `--type` and `--poke` script sessions.
- Sound: the PC speaker tones and engine note generated from the game's sound model (`sound.c`).

## Verification (the faithful gate)
- `python tools/verify_campaign.py` (emulator build in `build-emu/`): every campaign session with
  `--verify all` — each native runs next to the original code; must print `OK` with 0 mismatches.
  Area-specific campaigns: `tools/*_sessions.py`. Needs the disk image in `original/`.
- Enhancements are side channels: with `--classic` (and in the emulator build) the engine's memory,
  cycles and CGA bitmap must stay byte-identical to the faithful port.

## Build
```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cmake -S . -B build-emu -G Ninja -DCMAKE_BUILD_TYPE=Release -DFS1_EMULATOR=ON && cmake --build build-emu
```
Headless run: `SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy build/fs1-enhanced.exe --frames 120
--screenshot work/x.bmp`. Release zip: `python tools/package.py`.
