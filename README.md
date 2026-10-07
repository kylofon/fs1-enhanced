# FS1 Enhanced — SDL3

An enhanced native port of *Microsoft Flight Simulator* 1.05 (IBM PC, 1982, by Bruce Artwick /
subLOGIC), built on the faithful [SDL3 port](https://github.com/kylofon/fs1-sdl3-port), in the making.
Every routine of the original runs as C; the enhancements draw it sharply at any size and add modern
controls, while `--classic` plays exactly as the faithful port. The original disk image is not included.

Status and plan: [PLAN.md](PLAN.md) (as a page: `python tools/plan_site.py`, then open
`site/index.html`). Agent rules: [CLAUDE.md](CLAUDE.md). Engine rules: [ENGINE.md](ENGINE.md).

## Features

* `--classic`: the original picture and behaviour, as the faithful port v0.1.0.
* `--view-border`: keep the white left border of the 3D window (the sharp view leaves it out).
* `--smooth on|ahead|off` (default `on`): the game draws its 3D view about five times a second; the sharp
  view moves it on at every display frame, between the last two pictures (`on`, one picture late) or past the
  last one (`ahead`, no delay). The simulation is unchanged.
* More to come (PLAN.md): sharp 3D view, panel and text; widescreen; smooth frame rate; gamepad and
  mouse yoke; key bindings, settings, quick save; a launcher.

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
