# FS1 Enhanced — SDL3

An enhanced native port of *Microsoft Flight Simulator* 1.05 (IBM PC, 1982, by Bruce Artwick /
subLOGIC), built on the faithful [SDL3 port](https://github.com/kylofon/fs1-sdl3-port). Every routine of the
original runs as C; the enhancements redraw it sharply at any window size and move the 3D view smoothly,
while `--classic` plays exactly as the faithful port. The simulation itself is unchanged. The original disk
image is not included.

Status and plan: [PLAN.md](PLAN.md) (as a page: `python tools/plan_site.py`, then open
`site/index.html`). Agent rules: [CLAUDE.md](CLAUDE.md). Engine rules: [ENGINE.md](ENGINE.md).

## Play (Windows release)

Unzip `fs1enhanced-vX.Y.Z-win64.zip` from the [releases](https://github.com/kylofon/fs1-enhanced/releases),
start `FS1 Enhanced.exe` (the launcher, see [launcher/README.md](launcher/README.md)), pick your 160K PC
booter disk image of Flight Simulator 1.05 (`.ima`) and a start location, and press **Play**. At the game's
start-up menus press **A** and **B**. Or put the image next to `fs1-enhanced.exe` (or in `original/`) and start
that directly. Keys: as in the faithful port's [README](https://github.com/kylofon/fs1-sdl3-port#readme).

## Features

* **Sharp 3D view**: the scenery the game draws is recorded as vectors and drawn at window size, with filled
  sky, ground and polygons (`--line-width F`, default 2; `--view-border` keeps the white left border).
* **Vector cockpit**: every instrument (airspeed, attitude, altimeter, OMI/VOR, turn coordinator, heading,
  VSI, tachometer, the right-hand panel and the radio stack) and the panel frame are redrawn as vectors from
  the game's values; no CGA pixels are left in the panel (`--module-off NAME|all` shows the old look).
* **Sharp text**: menus, panel text, labels and the editor in a scalable font.
* **Smooth motion** (`--smooth on|off`, default on): the game draws its 3D view about five times a second;
  the sharp view moves it on at every display frame, between the last two pictures (one picture late).
* **Launcher** (`FS1 Enhanced.exe`): disk image, start location, line width, smooth motion, RGB, sound.
* `--start N`: recall the game's user mode N (0-9) once the start-up menus are over (2 = the demo flight).
* F11 cycles the sharp, composite and RGB monitors; `--classic`: the original picture and behaviour, as
  the faithful port v0.1.0.
* `--record FILE [--record-from N]`: every frame to a 60 fps H.264 video (needs `ffmpeg` on the PATH), e.g.
  with `--frames`.

## Build and run (Windows, MSYS2 MinGW64)

```bash
export PATH="/c/msys64/mingw64/bin:$PATH"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/fs1-enhanced.exe
```

Put your 160K PC booter disk image (`.ima`) in `original/`, next to the exe, or pass its path. Keys and
dev options: as in the faithful port's README. Verification build: `-DFS1_EMULATOR=ON` in `build-emu/`,
then `python tools/verify_campaign.py` (see [ENGINE.md](ENGINE.md)).

## Layout

* `src/` — the faithful engine (`ENGINE.md`; changes marked `ENH:`).
* `src/enhanced/` — the enhancements.
* `tools/` — state generator, verification campaigns, packaging, plan page.
* `docs/` — symbols, program map, scenery format (from the faithful repo).

## License

MIT ([LICENSE](LICENSE)). *Microsoft Flight Simulator* is © Microsoft / subLOGIC; it is not included.
