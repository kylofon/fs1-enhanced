# Changelog

## Unreleased

- Project set up from the faithful port v0.1.0 (`../Flight Simulator 1`, commit fdb9bf1).
- `--classic`: every enhancement off (there are none yet).
- Cockpit module framework (S2.5): the panel is divided into instrument modules that will each be redrawn whole as
  vectors (`--module-off NAME|all`, `--module-outline`); see docs/PANEL.md.
- Airspeed indicator (S2.6) redrawn as a vector module: ring, ticks, numerals, KNOTS and the needle from the game's airspeed.
- Attitude indicator (S2.7) redrawn as a vector module: sub-pixel horizon from the game's bank and pitch, roll scale and pointer, aircraft symbol.
- Altimeter (S2.8) redrawn as a vector module: scale, numerals, long and short hands from the game's altitude.
- OMI/VOR indicator (S2.9) redrawn as a vector module: course needle, glide-slope bar, TO/FROM, marker lights, OBS numbers.
- Turn coordinator, heading indicator (with a turning card), vertical speed indicator + elevator strip and tachometer (S2.10-S2.13) redrawn as vector modules.
- Sharp panel (S2.1): the airspeed, altimeter, VSI and tachometer needles are drawn as vector lines at window
  size; the pixel needle sprites are taken out of the picture (`--needle-width F`, `--dump-panel FILE`).
