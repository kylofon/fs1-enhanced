# Enhanced renderer: display list (design, S1.1)

The natives keep drawing into the CGA memory exactly as the faithful port. With the sharp renderer on,
they also **record** what they draw — lines, dots, fills, polygons, later text and panel items — in
resolution-independent form. A new SDL back end draws that record at window size. Recording is a side
channel: it never writes emulated memory, never charges cycles and never changes a register.

Code: `src/enhanced/displaylist.h` (format and hook API), `src/enhanced/displaylist.c` (recorder).
Subtasks: S1.2 records the 3D view, S1.3 draws it, S2.1/S2.2 add the panel and text layer.

## 1. Rules

- **Off means off.** `dl_on` is false under `--classic`, in the emulator build while `--verify` or
  `--native-off` is active, and while the sharp renderer is not selected. Every hook is a static
  inline `if (dl_on) dl_xxx(...)`; with it off the engine runs exactly as the faithful port.
- **Read-only.** The recorder reads emulated memory through `pc->cpu.mem[]` directly, never through
  `cpu_read8`/`mem_read16` (those set trace bits that `--verify` compares). It never writes memory,
  registers or `cycles`.
- **One-line hooks.** Each engine edit is one call marked `/* ENH: display list */`. The hook passes
  `Pc *` and at most the registers the native already has in hand; the recorder reads the rest
  (eye coordinates, colours, patterns) from the game's own variables.
- **Route by target segment.** A drawing routine writes either the 3D back buffer (`ES == [3806]`,
  normally 0C0C) or the screen (`ES == B800`). Hooks pass the segment; the recorder files the item into
  the **view list** or the **screen layer** accordingly. The same native (e.g. `draw_line`,
  `plot_pixel`, the XOR helpers in `gauges.c`) may write either.
- **Never wrong, at worst blurry.** Whatever the recorder did not capture is shown from the CGA
  picture, scaled (section 5). A missing hook makes a region pixelated, never wrong or missing.

## 2. Coordinates

- **View space** (3D view items): x in [0, 160), y in [0, 106), units of the original's fat pixels
  (4 hi-res pixels × 1 scan line). The original pixel n covers [n, n+1). The view is placed on the
  640×200 screen at scan line `row_offsets[0]`'s row (0 in flight: the view is screen lines 0..105,
  full width); the frame stores that origin.
- **Sub-pixel points from the projection.** The original projects an eye-space point (x, y, z) as
  `s = centre + (int8)scale * v / z` with truncations (`project_axis` in `scenery.c`, `project` in
  `transform.c`): `sx` with scale `[3167]`, centre `[3169]`; `sy` with `[3168]`, `[316A]` (scale is
  negative for y). The recorder evaluates the same formula in float on the same clipped eye-space
  words, so `floor(s)` is the original pixel (±1 where the original's double truncation differs) and
  a sharp line lies within the fat pixels the original lit. The clip step's normalising shifts cancel
  in v/z.
- **Integer points** (screen-space byte lists, `plot_pixel` called directly, panel items) are
  recorded as pixel centres n + 0.5 and flagged `DL_F_INT`.
- **Screen space** (screen layer): 640×200 hi-res pixels, same [n, n+1) convention. The back end maps
  both spaces to the 4:3 output rectangle (D3: letterbox/pillarbox, no widescreen).

## 3. Item format

A frame is an array of fixed-size tagged items in draw order plus a pool of polygon vertices. In
memory only; `--dump-dl` (S1.2) writes one text line per item.

| Kind | Fields | Colour field | Drawn by the original as |
|---|---|---|---|
| `DL_FILL` | rect x, y, w, h (view or screen units) | word pattern | `clear_view_buffer`, `fill_rows`, `draw_view_frame`, overlay spans, radar clear |
| `DL_SKYGROUND` | horizon a, b (float); `pat_l`, `pat_r` (pattern on the left / right of a→b) | – | `horizon_fill` bands + `fill_sloped_horizon` |
| `DL_LINE` | a, b | nibble pair `[30C8]` | `draw_line` (normal path, and the polygon path's outline) |
| `DL_DOT` | p | nibble pair `[30C8]` | `plot_pixel` / `project_dot` |
| `DL_POLY` | `first`, `count` vertices in the pool (one or more closed rings) | colour byte `[3159]` | `poly_fill` flood fill |
| `DL_TEXT` (S2.2) | screen offset, string (≤ 40 chars inline), mask, shift | – | `print_str` family (`textdraw_string`) |
| `DL_SPRITE` (S2.1) | screen offset, size, sprite data address, op (OR/copy) | – | `blit_or`, `blit_sprite` |
| `DL_RESTORE` (S2.1) | screen rect | – | `indicator_erase`, needle restore (copies from the clean panel) |
| `DL_NEEDLE` (S2.1) | descriptor, angle 0..FF | – | `draw_needle`, `draw_needle2`, turn needle |

Flags: `DL_F_INT` (integer coordinates), `DL_F_EDGE` (a polygon's outline line), `DL_F_CAPTURED`
(drawn from a captured list).

**Colours are the game's patterns, not RGB.** A line or dot carries the nibble pair (4 hi-res bits per
fat pixel); a fill carries the 16-bit dither word (`7777` sky, `BBBB` ground, `2222` water …). The back
end turns a pattern into a colour by decoding a uniform field of it through the existing composite (or
RGB) decoder and averaging, cached per pattern. D1's "slightly richer" palette is then a small table of
per-pattern adjustments on top, never a different scheme.

**Limits.** 8192 items and 16384 vertices per view frame (a busy frame has a few hundred lines). On
overflow the frame is marked `incomplete` and the back end shows that frame from the CGA picture.

## 4. Recording points (3D view, S1.2)

Projection hooks only **stage** float coordinates together with the integer endpoints the original
computed. The drawing hooks then **emit** items: if the integers they are about to draw match the
staged ones, the staged floats are used, otherwise the integers (`DL_F_INT`). So nothing is recorded
twice, and anything drawn without a projection still appears.

| Where (native, file) | Hook | Records |
|---|---|---|
| `f_4D18` / `f_4CA3` (`scenery.c`, after `rSI = rCX`) | `dl_stage_p1(pc, from_prev, sw)` | float point 1: eye 310F/3111/3113, or for `f_4CA3` the previous staged end (checked against `[3165]`) |
| `f_4D56` (after `[3165]` is written) | `dl_stage_p2(pc, sw1, sw2)` | float point 2 (eye 311B/311D/311F) and the integer endpoints; the end is kept as the next "previous" |
| same, capture branch (`capture_mode` set) | `dl_capture(pc, addr)` | the staged line into the **capture shadow**, keyed by the capture address (horizon programs write `horizon_list` 041F) |
| `project` (`transform.c`, after both axes) | `dl_stage_dot(pc, x, y, z, sx, sy)` | float dot for the integer pixel (sx, sy). Capture-mode dots (FE records) are not shadowed: they are plotted as integer dots |
| `draw_line_cpu` (`line.c`) | `dl_line(pc, regs, seg, poly)` | `DL_LINE` from BX,BP → SI,DI with `[30C8]`; with `[31D0]` set it is also an edge of the open polygon (`DL_F_EDGE`) |
| `n_draw_line_list` (`line.c`), per record | `dl_stage_from_capture(pc, addr)` | looks the record up in the capture shadow (verified against the record bytes), so the dusk horizon line is sharp too |
| `plot_pixel` / `plot_pixel_es` (`buffer.c`) | `dl_dot(pc, x, y, seg)` | `DL_DOT` (staged float if it matches) |
| `n_clear_view_buffer` (`buffer.c`) | `dl_fill_view(pc, pattern)` | `DL_FILL` of the whole view; drops the items before it |
| `fill_rows_body` (`horizon.c`) / `n_fill_rows` (`buffer.c`) | `dl_fill_rows(pc, first, last, pattern, seg)` | `DL_FILL` of whole rows |
| `horizon_fill_body` (`horizon.c`, a wrapper around `horizon_fill_main`) | `dl_sky_begin(pc)` / `dl_sky_ground(pc)` | replaces the fills it made with one `DL_SKYGROUND`: horizon from the capture shadow of 041F; the pattern on each side read back from the back buffer at the view corner farthest from the line on that side (robust against the band logic and the inverted cases). No horizon (`[041F] == FF`) → the single `DL_FILL` stays. |
| `h_poly_begin` / `h_poly_end` (`scenery.c`), `f_span_fill` (at L59E9) | `dl_poly_begin`, `dl_span_fill`, `dl_poly_end` | begin reserves a `DL_POLY` slot **before** the outline lines (fill under the outline); end closes the rings from the edges recorded in between. Kept only if the flood fill filled something (`f_span_fill` counts); colour `[3159]`. |
| `n_draw_view_frame`, `overlay_body` (`mainloop.c`), `radar_clear_body` (`horizon.c`) | `dl_fill_bytes(pc, seg, off, n, value, mask)` | `DL_FILL` spans of the border (`F0` mask = one pixel), the overlay marks and the radar marks; `n_clear_view_buffer` and `radar_clear_body`'s clear use `dl_fill_view(pc, pattern, words)` |
| `n_blit_view_to_screen` (`buffer.c`) | `dl_commit_view(pc)` | ends the view frame: the built list becomes the committed one (double-buffered; S1.3 adds the snapshot of the view bytes for the CGA fallback, section 5). The next list starts empty. |

Polygon note: the original draws the outline, leaves the midpoint pixel of each edge as a seed and
flood-fills from the inside neighbours, replacing pattern `[31CE]` by `[3159]`. The sharp version fills
the vector polygon formed by the recorded edges; where clipping cut the outline, the ring is closed
along the view border between the two cut points. A flood fill that leaked or was blocked by earlier
lines cannot be reproduced; `--dl-check` (S1.2) reports polygons whose filled byte count differs much
from the vector area so such scenery can be listed.

Paths the natives hand to original code (scenery bail-outs, `--native-off`) are not recorded; the
fallback in section 5 shows them.

**S1.2 as built.** Hooks run *before* the native writes (except `dl_sky_ground`, `dl_poly_end`,
`dl_commit_view`), so `--dl-check` can attribute writes. Differences from the plan above:
- `--dump-dl FILE` writes one text block per committed frame (`frame N …`, then `fill`, `sky`, `line`,
  `dot`, `poly` lines; lines are listed as `a b` where a is often the far end: the draw routines receive the
  end points in either order). `--dl-check` prints, at exit, the pairs of hooks between which back-buffer
  bytes changed that no hook claimed, plus a polygon fill-vs-vector report. Both options turn `dl_on` on
  (never under `--classic`); the verification gate never passes them.
- **Polygon rings** are built from the outline edges by joining touching end points (either direction), then
  chains cut by the clip rectangle (x 1..159, y 0..105) are joined along the view border; a ring left open
  is closed by a chord, or through one corner when its ends are on adjacent borders. Rings without area
  are dropped. Rings in the vertex pool are separated by a NaN vertex.
- **Leaking flood fills.** A fill that is not bounded by its outline (mountain polygons whose outline is only
  the ridge line fill the sky down to the horizon) cannot be reproduced from vectors. `dl_poly_end` compares
  the pixels changed since `dl_poly_begin` with area + perimeter; when it is more than twice that the
  `DL_POLY` gets `DL_F_LEAK` and the box `x0,y0-x1,y1` of the changed pixels. S1.3 shows that box from the
  CGA picture. In the campaign sessions about 20% of the polygons in the demo leak, and most in `editor_values`.
- Not recorded (listed by `--dl-check` only when they happen): the war-mode gun-sight clear in
  `overlay_body` (AND masks), the resumable loop natives `n_view_frame_loop` / `n_overlay_rows` (an interrupt
  was due inside the frame/overlay), `horizon_list_body` (native off) and text/panel items (S2).
- The campaign sessions (demo, flight_keys, slew, war, editor_values, user modes, display types) all end with
  "every back-buffer write was recorded" except `reboot`, whose first frame is the boot text screen.

## 5. Frames, validity and the CGA fallback

- **View list.** Built between two `blit_view_to_screen` calls, committed at the blit. The back end
  always draws the last committed list; the simulation's frame rate is unchanged (S3.2 moves its
  points on to the display frame's time, section 6d).
- **Screen layer** (S2.1/S2.2). The panel and text are drawn straight into B800 at irregular times and
  stay there, so this layer is **retained**: a list of items, each owning the screen bytes it wrote.
  Two 16 KB arrays shadow B800: `owner[byte]` (item id, 0 = none) and `expect[byte]` (the value right
  after the owner wrote it). After an item's native returns, the recorder claims the bytes of its rect.
  An item that no longer owns any byte is dropped. The committed view is one item owning the view rows.
- **Validity check at present time.** For every tile (4 bytes × 2 scan lines = 16×2 hi-res pixels) the
  back end compares B800 with `expect`. A tile is **sharp** if every byte has an owner and matches;
  otherwise it is a **fallback** tile and is drawn from the scaled CGA picture (the existing composite /
  RGB decode) on top of the sharp layer. So the startup screens, the static panel art, unhooked writers
  and anything an unrecorded path overwrote simply appear as in the authentic mode, tile by tile.
- **`--dl-check` (debug, S1.2).** Snapshots the back buffer around every hooked writer and, at commit,
  lists back-buffer bytes changed by a writer without a hook (with the native that was running). This is
  how the hook list above is proved complete.

## 6. Back end contract (S1.3)

Per display frame:
1. Clear the output to black; compute the 4:3 output rectangle.
2. Draw the committed view list in order: fills and `DL_SKYGROUND` (the view rectangle split by the
   infinite line through a, b; two `SDL_RenderGeometry` polygons), `DL_POLY` (triangulated rings),
   lines (`SDL_RenderLines` or quads for thickness ≥ 1 output pixel, width option), dots (squares of one
   fat pixel's height). Clip to the view rectangle.
3. Draw the screen layer items (S2), then the fallback tiles from the CGA texture.
4. F11 cycles composite → RGB → sharp (D2: sharp is the default; `--classic` forces authentic).

## 6b. Panel needles, S2.1 as built

The panel did not need the retained screen layer of section 5 (owner/expect shadows, hooks in
`indicator.c`): the needle drawer keeps everything needed in the game's own data, so the back end
**reads state instead of recording** and the engine gets no `ENH:` hook for it (only `pc_render_cga`
takes the video RAM image as a parameter, and `main.c` calls `sharp_render_cga` while sharp is on).

Each frame (`sharp_render_cga`, `src/enhanced/sharp.c`), for the five needle descriptors 092F airspeed,
0939 / 0943 altimeter large / small hand, 094D VSI, 0957 tachometer (docs/subphases/3.10.md of the
faithful port):

1. Read code (+4), screen offset (+5), size word (+7); take the sprite from the table (0961 for
   `draw_needle`, 098F for `draw_needle2`, the other one if the size word disagrees) and rebuild the bits
   the needle drawer ORed (rows go up or down from the hub by the code's quadrant bits, left-half sprites
   mirrored through the table at DS:12B4, same row-step arithmetic as `needle_draw_normal/mirrored`).
2. **Validity.** The needle counts only if every one of those bits is set in B800. A stale descriptor, a
   half-drawn or erased needle, a menu or editor screen all fail this and stay as the CGA picture.
3. The bits are cleared in a copy of the video RAM and replaced by the clean panel's (segment at
   0000:0122 is a copy of the panel with the same offsets as B800); the CGA texture is decoded from that
   copy, so it shows the panel without the needles.
4. Each needle is a line fitted to its sprite pixels in output space (640x480, hi-res pixel = 1 x 2.4):
   principal axis through the pixel centroid, ends at the extreme pixels, `a` the end nearer the hub byte;
   thickness = pixel area / length (`--needle-width F` scales it); colour = the composite colour of the
   sprite's most frequent nibble (white). `sharp_view` draws it as a quad that narrows towards the tip
   over the unclipped panel.

Not vectorised (they stay crisp CGA pixels): the control-position indicators (`draw_indicator`: aileron,
rudder, elevator, throttle, flaps, trim), the altimeter band marker, the turn indicator (a sprite that
slides, `turn_needle`), the artificial horizon (`attitude`: rows 76h..98h, row records at DS:1BAB, bank
dots), radio digits and text (S2.2). `--dump-panel FILE` writes one line per needle per frame (fitted
ends, width, colour) for checks.

## 6c. Text, S2.2 as built

One hook: `textdraw_string` (`textdraw.h`) calls `dl_text_row(es, di, w)` for every glyph row it writes; only B800 is
kept (`text_w[off]`, `text_ok[off]` in `displaylist.c`). Each frame `text_collect` (`sharp.c`) keeps a row only if the
video RAM still holds exactly its two bytes, zeroes those bytes in the CGA copy and puts their nibbles into a 160x200
grid of fat pixels. `text_draw` (after the view and its fallback tiles) draws every non-zero cell as a rectangle of
4 x 2.4 output pixels with the convex corners cut (30%), and a small rhombus where two cells touch only diagonally;
colour = uniform-field composite decode of the nibble. The font itself is 4 x 5 fat pixels per glyph, so this is a
smoothing of the game's own glyph shapes, not a different font. Screens without a panel (menus, editor) take the same
path.

Text that reaches the screen another way (drawn into a buffer and blitted, e.g. flight status and radio text) is
not seen by the hook, so `text_detect` also looks for the game's glyphs in the video RAM (five stacked rows equal to a
glyph's, masks FFFF/8888, below the 3D view only in flight) and treats a match like a recorded row.

Glyphs found this way are drawn with the 8x8 text-mode font (`pc->font`, the look of the ESC screen) instead of the
game's thin 16x5 shapes: white (grey when dim), one rectangle per run of lit pixels, 2 x 1.7 output pixels per font
pixel. Rows the detection did not claim (shifted text, panel art) keep the smoothed-cell drawing above.

## 6d. Smooth frame rate, S3.2 as built

The game commits a new view about every 200-260 ms (4-5 per second, measured over the campaign sessions). The
back end draws the view at every display frame from the last committed frame with each 3D point moved to the
display frame's time (`dl_view_at(now)`); the simulation, its rates and the CGA picture are untouched (cycles
identical to `--classic`).

**Time base.** `now` is the display clock (`target_cycles` in `main.c`: the emulated time the display frame stands
for), counted from the display frame that first showed the view. The CPU cycle counter is no clock for this: the
scenery pass is one native step that charges its ~0.9 M cycles (~0.2 s) at once, and the next ~11 display frames
run no CPU to pay it back, so "cycles since the commit" jumps from 0 to a whole interval in two display frames
(the first version did that: no visible change).

**Vertices, not poses.** The scenery switches eye position, angles, local origins (op 24, with a shift) and even
projection scales (op 0E) many times per frame, so one camera pose per frame cannot re-project it. Instead every
point the scenery puts into eye space becomes a **vertex** (`DlVtx`): its eye vector in float, in the units of the
clip and the projection (rotate_point's rows x, 2y, z/2: the view is |x| <= z, |y| <= z), and an **identity**
(scenery address of the point, + step number, + occurrence in the frame). At the commit each vertex looks for its
identity in the previous committed frame and keeps that position as `e0` (unless implausibly far: further than
its own distance). At draw time each vertex is at `e0 + (e - e0) * s`, `s` = display time since the view first showed / CPU
time between the last two commits (clamped to 0..1), or `1 + s` with `--smooth ahead`; lines are clipped against the view
pyramid in float, projected with the item's projection set (`DlFrame.pj`, [3167]..[316A] at the time), dots are
dropped outside, the horizon line is only clipped at a small z, and every polygon whose edges all have vertices is
rebuilt from its re-projected edges with the same ring builder (`build_rings`) as at record time. Items outside the
view at that moment get `DL_F_HIDDEN`. Items without vertices (fills, 2D lines, `DL_F_INT` items, leaking
polygons' CGA boxes, fallback tiles) stay where the frame has them.

**Eye vectors without the game's scaling.** world_to_eye_delta and rotate_point scale every point by its own power
of two (to fit 16 bits), which changes with distance from frame to frame, so the words cannot be interpolated. The
float vector is computed from the raw point instead:

| Hook | Where | Does |
|---|---|---|
| `dl_eye_delta(pc, start, end, x, y, z)` | `world_to_eye_delta` (`transform.c`, at `store`) | raw point at DS:start (y from the record, or [315B] when only x, z were read: end - start = 4) minus `eye_pos`; pending, with the scaled words it stores |
| `dl_rotated(pc)` | `rotate_point` (`transform.c`, before the return) | if `xform_in` holds the pending words: vertex = view matrix x the raw delta, identity = point address; otherwise (war planes) the delta as it is, no identity. Registered under the eye words BX, CX, DX |
| `dl_steps_begin(P, rSI)` | dotted/dashed set-up `f_43B4` (`scenery.c`, end) | the steps start at the second end point (DS:SI-6): its float vector over its words gives the scale of every step point |
| `dl_step(P)` | `f_4428` (`scenery.c`, end) | step point 31B5.. = words x that scale, identity = opcode address + step number |
| `dl_line_begin(P)` | `clip_project_line` (`scenery.c`, entry) | looks both ends (310F, 311B, not clipped yet) up by their words; `dl_stage_p2` hands them to the line |

Lines and dots find their vertices by the **eye words**: a point keeps the words rotate_point gave it while it is
copied around (move start 3133, previous end 3127, cache slots 3288..), so no hook is needed at the copies; the
latest point with the same words wins. `dl_stage_dot` looks up its x, y, z words the same way; the capture shadow
keeps the staged vertices (the horizon line), valid only within the frame they were staged in.

**Frame changes.** No interpolation (`dt` = 0) after the first frame, more than 1.5 s between commits (menus,
pauses, scenery loads), or a change of the view direction / radar view ([059B], [0405]).

**Checks** (`--dl-check`, at exit): `smooth: N frames, T ms apart, P% of V vertices found in the previous frame`
(96.7-100% in every campaign session) and `re-projected at their own time, K of M items are off by more than 1.5 px`
(the frame drawn at s = 1 against the recorded floats: 0 in most sessions; the few are long ground-polygon edges
that start far behind the eye, where the game's fixed-point clip is a few pixels off the float one).
`--dump-dl` lists `vtx`, `matched`, `t` and `dt` per frame.

## 7. Later subtasks

- **S2.2** fills in the hooks for `war.c` (scope 334A/3308, status line) and `text.c`, using the retained
  screen layer of section 5 (S2.1 did the needles without it, section 6b); the artificial horizon
  (`gauges.c`: `attitude`, span fill 293F, XOR bank dots 5B88/2A3E) is still to do.
