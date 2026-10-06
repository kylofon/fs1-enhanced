# Cockpit modules (S2.5)

The instrument panel is redrawn one **module** (instrument) at a time (PLAN.md Phase 2b). Code:
`src/enhanced/panel/cockpit.{h,c}` (registry, blanking, frame state) and `src/enhanced/panel/kit.{h,c}`
(vector kit and panel font). Hooks: `sharp_render_cga` and `sharp_view` in `src/enhanced/sharp.c` only;
no engine code is touched, `--classic` is unaffected.

## How a frame goes

1. `sharp_render_cga`, in flight only (the 3D view list is complete): `cockpit_begin(pc, clean)`.
2. The older paths report what they find and leave anything inside a **drawn** module alone
   (`cockpit_owns`):
   - S2.1 needles: `cockpit_add_needle(desc, hub, tip)`; owned needles are not drawn by `panel_draw`.
   - S2.2 glyphs found in video RAM: `cockpit_add_text(x, line, ch, dim)`; owned ones are not drawn.
   - S2.4 traced labels: an owned cluster is not traced.
3. `cockpit_end(vram)`: every drawn module's rect is cleared in the CGA copy (the whole rect, so its
   bezel, scale, labels and fringes are gone; the module draws all of it).
4. `sharp_view` → `cockpit_draw`: each drawn module's `draw(frame, module)`, then `--module-outline`,
   one `SDL_RenderGeometry` batch on top of everything else.

A module is **drawn** when its `draw` is set in the table in `cockpit.c` and it is not switched off
with `--module-off`. Modules without `draw` keep the older look, so the panel improves one module at a
time.

## Options

- `--module-off NAME[,NAME...]` / `--module-off all`: the module(s) show the older look again (for
  side-by-side checks and bug hunting).
- `--module-outline`: every module rect as a box, green = drawn, orange = not done yet.

## Coordinates

Panel units: x in hi-res pixels (0..640), y in CGA scan lines (0..200), floats. The output is the
640x480 logical screen: x 1:1, y x 2.4 (`KIT_SY`). A round instrument is therefore an ellipse in panel
units: `rx` in pixels, `ry` in lines (on screen `ry * 2.4` output pixels). Rects are inclusive.

## Vector kit (`kit.h`)

`kit_line`, `kit_box`, `kit_fill_box`, `kit_arc` / `kit_ellipse` / `kit_fill_ellipse` (angles in degrees,
0 = up, clockwise, like a dial), `kit_ticks` (n ticks from a0 to a1), `kit_dial_point`, `kit_needle`
(tapered), `kit_text` (the 8x8 ROM font as runs of quads: cap height in output pixels, any rotation,
left/centre/right aligned). Palette: `kit_white`, `kit_grey`, `kit_black`, `kit_face`, `kit_bezel`, and
`kit_composite(nibbles)` for the composite colour of a CGA pattern (the same decode as the sharp view,
+20% saturation, D1). Line widths are output pixels.

## Frame state for modules (`CockpitFrame`)

- `pc` (memory, state accessors via `pc->mem`), `clean` (the clean panel image at cseg:0, B800 layout).
- `needle[]`: desc 0x92F airspeed, 0x939 altimeter large, 0x943 altimeter small, 0x94D VSI, 0x957
  tachometer; hub and tip in panel units (fitted to the sprite, S2.1). Better: read the value the
  needle shows from the game state and compute the angle (module subtasks decide).
- `text[]`: game glyphs found in video RAM this frame (S2.2): top left, character (0x21-0x5F), dim.
  The radio stack, compass and OMI read their digits from here.

## Layout (measured 2026-10-06 from the clean panel, frame 910 on the runway)

Measured as the bounding boxes of the large pixel clusters (gauges) and of the long horizontal and
vertical runs (box frames) in the clean panel. Each module subtask refines its rect and measures
centre/radius/scale angles itself.

| Module | Rect x0,y0 - x1,y1 | Notes |
|---|---|---|
| airspeed | 4,113 - 106,168 | gauge cluster 5..105 x 123..167; top merges with the glareshield arc (lines 108-112) |
| attitude | 109,113 - 211,158 | bezel 110..210 x 113..157 |
| altimeter | 212,113 - 316,158 | bezel 213..315 x 113..157 |
| omi | 317,113 - 421,158 | bezel 318..420 x 113..157, inner card 336..404 x 121..149 |
| turn | 12,169 - 100,199 | 13..99 x 169..199; inclinometer box 23..88 x 187..194 |
| heading | 116,163 - 203,199 | 117..202 x 163..199 |
| vsi | 204,163 - 308,199 | gauge 223..307; elevator trim strip 211..213 x 165..186 |
| tach | 328,163 - 412,199 | 329..411 x 163..199 |
| throttle | 428,109 - 441,199 | between the double lines at x 425/427 and 442/444 |
| compass | 445,109 - 514,141 | COMPASS, digits, OMI lights; box lines x 444/515, y 108/142 |
| fuel | 518,109 - 636,141 | E-F boxes 522..574 / 580..632 x 116..124 and 126..134, OIL T / OIL P |
| status | 541,143 - 636,169 | GEAR / LIGHTS / MAGS / CARB H |
| com | 445,143 - 537,155 | box lines y 142/156, x 444/538 |
| nav | 445,157 - 537,169 | y 156/170 |
| transponder | 445,171 - 561,183 | y 170/184, x 444/562 |
| time | 445,185 - 561,197 | y 184/198 |

Not a module yet: the glareshield arc (lines ~104-113), the frame lines between the modules, the area
right of the transponder/time boxes (x 565..636 x 171..197). S2.19 takes these.

## Writing a module (S2.6-S2.19)

1. Measure its layout numerically from the clean panel (dump `cseg:0`, 16 KB, B800 layout; no images).
2. Write `static void draw_xxx(const CockpitFrame *f, const CockpitModule *m)` in a new file
   `src/enhanced/panel/xxx.c` (declared in `cockpit.c`), set it in the table, refine the rect.
3. Draw everything inside the rect: face, bezel, ticks, numerals, labels, moving parts.
4. Check: `--classic` cycle count unchanged; `--module-outline` and `--module-off xxx` for comparison;
   U1 check of that module.
