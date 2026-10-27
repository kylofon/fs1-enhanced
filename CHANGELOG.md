# Changelog

## 0.1.0 - 2026-10-27

First release, built on the faithful port v0.1.0. The simulation is unchanged; `--classic` plays exactly as
the faithful port.

- Sharp 3D view (S1.2, S1.3): the scenery is recorded as a display list and drawn as vectors at window size,
  with filled sky, ground and polygons (`--line-width F`, `--view-border`, `--dump-dl FILE`, `--dl-check`).
- Smooth motion (S3.2, `--smooth on|off`): the 3D view moves on at every display frame between the game's
  last two pictures; no blinks in turns, the horizon slides, shore lines reach the view edges.
- Sharp text (S2.2, S2.4): menus, panel text, labels and the editor in a scalable font.
- Vector cockpit (S2.1, S2.5-S2.19): every instrument, the right-hand panel, the radio stack and the panel
  frame redrawn as vector modules from the game's values (`--module-off NAME|all`, `--module-outline`,
  `--needle-width F`, `--dump-panel FILE`); see docs/PANEL.md.
- Launcher `FS1 Enhanced.exe` (S5.2): disk image, start location, line width, smooth motion, RGB, sound.
- `--start N`: recall user mode N once the start-up menus are over; F11 cycles sharp / composite / RGB.
- `--record FILE`, `--record-from N`: write every frame to a 60 fps video through ffmpeg.
