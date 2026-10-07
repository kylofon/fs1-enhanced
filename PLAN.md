# FS1 Enhanced — plan

Built on the faithful port (`../Flight Simulator 1`, release v0.1.0, commit fdb9bf1). The faithful
engine runs unchanged (`ENGINE.md`); enhancements live in `src/enhanced/` and hook into the engine at
marked `ENH:` points. `--classic` turns every enhancement off.

Goal: a modern Flight Simulator 1 — sharp graphics at any window size, widescreen, smooth motion,
gamepad, settings and a launcher — with the original simulation untouched.

Token efficiency is a first-class goal: small subtasks, one session each, the cheapest model that can
do the job, and the user doing what is cheap for a human (looking at pictures, playing).
Tracker: `python tools/plan_site.py` → `site/index.html` (this plan plus token usage per subtask).

## User to-do

What the agents need from you now, most urgent first. Agents add a row when they hand you a task and
remove it when it is done; subtasks marked `U<n> pending` are added automatically on the plan page.

| Task | What to do | For | Time |
|---|---|---|---|
| U1 | S3.2 smooth motion, 3rd look (fixed: far scenery that stood still and then slid to a new place; the game truncates the eye position in coarse scenery frames, the smooth view now keeps the fraction): close the game, `cmake --build build`, run `build/fs1-enhanced.exe`, fly a long way along the coast and over the lake. Reply `OK` or what is still wrong (where, and does it also happen with `--smooth off`, i.e. in the game itself) | S3.2 | 5 min |
| U1 | Cockpit follow-ups (first answer 2026-10-07, from a screenshot: the top-left corner under the view shows the ground down to the glareshield curve, whose edge is a staircase of pixel steps, not a curve; to fix in its own session: the frame module's glareshield curve as a smooth line, and the fill below the view bounded by that line instead of per-column steps): (1) in flight the sky and ground now continue behind the cockpit's top edge (the black strip under the view and the top-left corner above the glareshield curve; scenery objects are still clipped at the old view border); (2) the start-up menu screen shows the vector cockpit too. Rebuild, run next to `--classic`: look at the top-left corner of the panel in flight (horizon line crossing the gap, colours matching) and at the menu screens (A, B). Reply `OK` or what is wrong | follow-up | 3 min |
| U1 | S2.14-S2.19 right panel and frame (throttle/flaps/trim strips, COMPASS + O M I lights, FUEL/OIL gauges, GEAR/LIGHTS/MAGS/CARB H, radio stack COM/NAV/TRANSPONDER/TIME, panel frame): close the game, `cmake --build build`, run next to `--classic` (`--module-off all` = old look, or by name, e.g. `--module-off frame`), take off, press G, L, H, A etc. and tune the radios: every value should show and change as in classic. Reply `OK` or what is wrong per part (missing/wrong text, sliders moving the wrong way, boxes, lines missing at the panel edges or between gauges) | S2.14-19 | 10 min |
| U1 | S2.10-S2.13 bottom row (turn coordinator, heading indicator, VSI + elevator strip, tachometer): close the game, `cmake --build build`, run next to `--classic` (`--module-off turn,heading,vsi,tach` = old look), take off and fly: turn symbol tilts with the turn, ball moves; heading digits and the turning card; VSI needle, elevator marker, rudder marker/track at the bottom; tach needle. Reply `OK` or what is wrong per gauge (scale positions, directions, sizes, overlaps, anything missing) | S2.10-13 | 8 min |
| U1 | S2.9 OMI/VOR indicator: close the game, `cmake --build build`, run next to `--classic` (`--module-off omi` = old look), tune a NAV station (or just fly): ring and card, course needle, glide-slope bar, OFF/TO/FROM, the OBS course (3 digits above the card) and its reciprocal (below); the O/M/I lights are now in the COMPASS box. Reply `OK` or what is wrong (needle/bar position and direction vs classic, flag text, number positions) | S2.9 | 5 min |
| U1 | S2.8 altimeter: close the game, `cmake --build build`, run next to `--classic` (`--module-off altimeter` = old look), take off and climb: ring, 50 ticks, numerals 0-9, ALT, long hand (1000 ft/turn) and short thick hand (10000 ft/turn). Reply `OK` or what is wrong (hand angles vs classic, sizes, the small boxed label in the old dial that is NOT redrawn, see notes) | S2.8 | 3 min |
| U1 | S2.7 attitude indicator: close the game, `cmake --build build`, run next to `--classic` (`--module-off attitude` = old look), take off and bank/pitch (keys, or `--poke` in a test run): sky/ground split with a smooth horizon, bank pointer on the disc, fixed roll ticks on the ring, aircraft symbol. Reply `OK` or what is wrong (horizon position or tilt direction vs classic, which side is sky, pointer direction, sizes, colours) | S2.7 | 3 min |
| U1 | S2.6 airspeed module (numbers 15% smaller after your first look): close the running game, `cmake --build build`, run `build/fs1-enhanced.exe` next to `--classic` (`--module-off airspeed` shows the old look), take off (A, B at the menus, F2) and watch the airspeed dial (left): ring, ticks every 10 kt, numbers 40-200, KNOTS, needle. Reply `OK` or what is wrong (needle angle vs the numbers, size/overlap of the numbers, ring, colours, gaps in the glareshield line above) | S2.6 | 3 min |
| U1 | S2.4 sharp panel labels: close the running game, `cmake --build build`, run `build/fs1-enhanced.exe` next to `--classic`, take off (A, B at the menus) and look at the panel labels (COMPASS, FUEL, OIL, COM, NAV, TRANSPONDER, TIME, KNOTS, GEAR/LIGHTS/MAGS/CARB, gauge numerals): they should be crisp white strokes, same letters and place as classic, no leftover coloured dots. Reply `OK` or what is wrong (missing/odd letters, too thin, colour) | S2.4 | 3 min |
| U1 | S1.3 + S2.2 result 2026-10-06: menus and 3D graphics OK. Cockpit: values sharp, labels not (= S2.4), some values broken (right-hand panel column GEAR/LIGHTS/MAGS/CARB shows `bc`, `f g`, `l`; `yyy` under OMI; check heading `330`). Next session: fix broken values (S2.2 bug), then S2.4. Send a `--classic` screenshot of the same frame if the broken values differ from it | S2.2 | 3 min |
| U1 | S2.2 sharp text (re-check after the fix): close the running game, `cmake --build build`, run `build/fs1-enhanced.exe` next to `--classic` and look at the start-up menus, the flight status/radio text and the editor screens: the letters should be smooth at any window size, same colour and place as classic. Reply `OK` or what is wrong (blurry/missing letters, leftover pixel letters, wrong colour, text that does not vanish when the game erases it) | S2.2 | 5 min |
| U1 | S2.1 sharp panel needles: close the running game, `cmake --build build`, run `build/fs1-enhanced.exe` next to `--classic`, take off (A, B at the menus, F2 for throttle) and watch the airspeed, altimeter (both hands), VSI and tachometer needles; or open `work\sharp_p.bmp` vs `work\classic_p.bmp` (frame 900 on the runway). Reply `OK` or what is wrong (needle position/length, thickness: `--needle-width F`, colour, flicker, leftover pixel needles) | S2.1 | 5 min |
| U1 | S1.3 sharp 3D view: close the running game, `cmake --build build`, run `build/fs1-enhanced.exe` (sharp is the default) next to `--classic`, fly (A, B at the menus) and compare the 3D view; or open `work\sharp_650.bmp` vs `work\classic_650.bmp`. Reply `OK` or what is wrong and where (colours, line thickness `--line-width N`, runway/ground fills, blurry patches) | S1.3 | 5 min |

## Status

- **Done:** S0.1 (repo from the faithful port v0.1.0; `--classic`; verification gate OK), S1.1 (`docs/RENDER.md`), S1.2, S1.3 (U1 pending), S2.1 (U1 pending), S2.2 (U1 pending), S2.4 (U1 pending), S2.5, S2.6 (U1 pending), S2.7 (U1 pending), S2.8 (U1 pending), S2.9 (U1 pending), S2.10-S2.19 (U1 pending), S3.2 (U1 pending).
- **Next:** U1 checks (rows at the top); S3.3 (scenery behind the cockpit edge, asked for 2026-10-07); then Phase 4 (S4.1).
- Escalation rule: a subtask that fails twice on Sonnet → new Opus session with a 5-line note
  (symptom, file, what was tried). Never carry an old transcript over.

## Model guide

| Model (app model picker) | Effort | Use for | Relative cost |
|---|---|---|---|
| **Haiku 4.5** | low–medium | Packaging, launcher wiring of existing options, README/CHANGELOG/PLAN bookkeeping, regeneration (`gen_state.py`), verification runs | 1× |
| **Sonnet 5.5** | medium | Default: features on a finished design (back ends, panel, text, input, settings), bug fixes with a known cause | ~3× |
| **Opus 5.5** | high | Architecture (display list, render back end), anything touching the 3D projection/clipping or timing, cross-module bugs, escalations | ~5× |

## How to run a subtask (user, every time)

1. Code tab → **New session** in `C:\Coding\FS1Enhanced`. Never continue an old session for a new
   subtask: the old context is re-billed on every turn.
2. Pick the model from the subtask table in the model picker; set the effort.
3. Send exactly: `Do S1.1 from PLAN.md.` (with the subtask id). `CLAUDE.md` tells the agent what to read.
4. When the agent says it is done: glance at the commit (`git log -1 --stat`), do any U-check it asks
   for, run `python tools/plan_site.py` if it didn't, close the session.

## Subtasks

Model: H = Haiku 4.5, S = Sonnet 5.5, O = Opus 5.5. Size: rough session length (S/M/L).
Status goes in the row's last cell (`done YYYY-MM-DD`, `U1 pending`); longer notes go in the "Notes:"
list under each table, never between rows.

Rendering today: the natives draw into the 640×200 CGA bitmap, which is decoded to composite colour and
scaled. The enhanced renderer adds a **display list**: the same natives also record what they draw
(lines, fills, text, sprites) in resolution-independent form, and a new SDL back end draws it sharply at
window size. The CGA path stays as the authentic mode (`--classic`, or toggled in game).

### Phase 0 — Repository
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S0.1 | Repo from the faithful port at v0.1.0: engine, build (`fs1-enhanced`), verification tools, `src/enhanced/`, `--classic`, PLAN/CLAUDE/ENGINE/README | O | M | done 2026-10-03 |

### Phase 1 — Sharp 3D view
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S1.1 | Design: display-list format and recording points in the natives (`raster.h` lines, `fill_rows`/horizon, `textdraw.h`, `panel.h` sprites/needles, scenery dots); sub-pixel coordinates from the projection before rounding; `docs/RENDER.md`; no code beyond a skeleton in `src/enhanced/` | O | M | done 2026-10-03 |
| S1.2 | 3D view display list: record scenery lines/dots/polygons and horizon with float coordinates; `--dump-dl` to a text file per frame for checks | S | M | done 2026-10-03 |
| S1.3 | SDL back end for the 3D view: lines (`SDL_RenderLines`, thickness option), filled sky/ground and polygons (`SDL_RenderGeometry`), slightly richer palette than the 4 composite colours (minor adjustments for clarity only, D1); toggle authentic/sharp (F11 cycles with composite/RGB); **U1 check** | S | M | done 2026-10-03, U1 pending |

### Phase 2 — Panel and text
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S2.1 | Instrument panel at high resolution: needles as lines/polygons, indicators and sprites upscaled or vector-traced from the original sprite data; **U1 check** | S | L | done 2026-10-04, U1 pending |
| S2.2 | Text at high resolution: the 16×5 game font rendered as scalable glyphs (vector from the font bits), panel and menu text; editor text mode with a sharp font | S | M | done 2026-10-04, U1 pending |
| S2.3 | Artificial horizon at high resolution (`gauges.c` `attitude`: row records DS:1BAB, spans 293F, bank dots 5B88/2A3E): sub-pixel horizon line between the sky and ground colours, bank pointer; read the state like S2.1 (section 6b), verify against the video RAM; **U1 check** | S | M | superseded by S2.7 |
| S2.4 | Panel labels (COMPASS, FUEL, OIL, COM, NAV, TRANSPONDER, TIME, KNOTS, gauge numerals, GEAR/LIGHTS/MAGS/CARB) at high resolution, like S2.2: they are part of the panel bitmap (not the game font), so find them in the clean panel image (segment at 0000:0122) or from the panel art data, identify each character (own small glyph set of the panel font, or trace), redraw crisp in white/their colour with the 8x8-style font, remove the pixel/fringe version from the CGA copy; text only, gauge faces and scales stay | S | M | done 2026-10-06, U1 pending |

### Phase 2b — Cockpit, module by module
Change of approach (2026-10-06, after the S2.4 U1 look: letter-by-letter tracing gives an inconsistent
panel). Each instrument is now redrawn whole as one **module**: its CGA area is blanked in the CGA copy
and the back end draws the face (bezel circle/box), scale ticks, numerals and labels in one crisp vector
style, plus the moving parts from the state (needles from S2.1, values from S2.2). Modules not done yet
keep today's look (S2.1 needles, S2.2 values, S2.4 traced labels), so the panel improves one module at
a time. Every module subtask: measure the layout from the clean panel (cseg:0122) numerically, draw it,
`--classic` cycles unchanged, **U1 check** of that module only (side by side with `--classic`).

| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S2.5 | Module framework (`src/enhanced/panel/`): module registry (rect in hi-res pixels/scan lines, enable flag, `--module-off NAME`), blank the rect in the CGA copy, a vector kit (circle/arc, ticks along an arc, filled boxes, lines), one panel font (vector or 8x8-based, any size/rotation, white + panel colours from the composite decode), shared palette; measure and list every module rect (`docs/PANEL.md`); no module drawn yet beyond a debug outline (`--module-outline`) | O | M | done 2026-10-06, U1 OK 2026-10-06 |
| S2.6 | Airspeed indicator (KNOTS, 40-200 scale, needle) | S | M | done 2026-10-06, U1 pending |
| S2.7 | Attitude indicator (replaces S2.3: sky/ground split with sub-pixel horizon, bank marks and pointer, aircraft symbol) | S | M | done 2026-10-06, U1 pending |
| S2.8 | Altimeter (ALT, 0-9 scale, both hands, the small 100s/1000s window) | S | M | done 2026-10-06, U1 pending |
| S2.9 | OMI/VOR indicator (OBS card, CDI bar, TO/FROM, the 3-digit OBS window, bottom heading box) | S | M | |
| S2.10 | Turn coordinator (L/R marks, aircraft symbol, ball/inclinometer box) | S | M  done 2026-10-06, U1 pending |
| S2.11 | Heading indicator (DG card numerals, lubber line, digital heading box) | S | M  done 2026-10-06, U1 pending |
| S2.12 | Vertical speed indicator (UP/DN, 5/10/15 scale, needle) and the elevator trim strip next to it | S | M  done 2026-10-06, U1 pending |
| S2.13 | Tachometer (RPM, 5-35 scale, needle) | S | S  done 2026-10-06, U1 pending |
| S2.14 | Throttle/flaps strip (F/L/D/T column left of COMPASS: markers and bar) | S | S |done 2026-10-06, U1 pending |
| S2.15 | COMPASS box (magnetic heading digits) (the O/M/I lights are in the OMI module, done in S2.9) | S | S |done 2026-10-06, U1 pending |
| S2.16 | FUEL (two E-F gauges), OIL T and OIL P gauges | S | S |done 2026-10-06, U1 pending |
| S2.17 | GEAR / LIGHTS / MAGS / CARB H status column (labels + switch symbols, drawn as clear states, not the game's symbol glyphs) | S | S |done 2026-10-06, U1 pending |
| S2.18 | Radio stack: COM, NAV, TRANSPONDER, TIME boxes (labels, frames, digits in one font) | S | S |done 2026-10-06, U1 pending |
| S2.19 | Panel frame: glareshield edge, instrument bezels between modules, separators, the small knobs; then the whole panel without any CGA pixels left | S | M |done 2026-10-06, U1 pending |

### Phase 3 — View
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S3.1 | ~~Widescreen~~ dropped (D3): keep 4:3, letterbox/pillarbox at any window size, integer or smooth scaling option | S | S | dropped 2026-10-03 |
| S3.2 | Smooth frame rate: run the simulation at the original rates, render the 3D view every display frame with interpolated eye position/attitude from the display list; no sim behaviour change | O | L | done 2026-10-07, U1 pending |
| S3.3 | Scenery behind the cockpit edge: the strip under the view and the top-left corner above the glareshield show the 3D world, not only sky/ground fill. With S3.2 vertices the back end re-projects the scenery itself: clip against a frustum extended down to the lowest gap line (instead of the game's view pyramid), record the outline edges the game rejects as outside (seen at `clip_project_line` entry, no draw) so polygons such as the lake close correctly below the view, draw the extension clipped to the gap area; the glareshield curve drawn as a smooth line and the gap bounded by it (no pixel staircase). Scenery blocks the game culls as out of view stay missing there (known limit). No sim change; **U1 check** | O | M | |

### Phase 4 — Input
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S4.1 | Gamepad and joystick (SDL3 gamepad API): analog elevator/aileron/rudder/throttle written into the control variables with the original key steps' scaling; dead zone; hot-plug | S | M | |
| S4.2 | Mouse yoke (optional, toggle key) and configurable key bindings (`fs1.ini`); in-game help overlay listing the keys | S | M | |

### Phase 5 — Settings, launcher, release
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S5.1 | Settings and save state: `fs1.ini` (display mode, aspect, scale, audio, controls), F5/F9 quick save/load of the whole game state (memory array + registers) | S | M | |
| S5.2 | Launcher "FS1 Enhanced" (required, like the other ports) from `../StreetRodEnhanced/launcher/` (wxWidgets), adapted: disk image instead of a game folder, all options of S1–S5.1 (palette, scaling, smooth frame rate, gamepad, audio), key bindings; no "classic" entry (D2); built by default, `-DFS1_LAUNCHER=OFF` to skip | S | M |partial 2026-10-04: `launcher/` (disk image, start location = user mode via `--start N`, line width, RGB, sound); the other options when their subtasks exist |
| S5.3 | First release: `tools/package.py` (repo URL, launcher in the zip), README with screenshots taken by the user, CHANGELOG, make https://github.com/kylofon/fs1-enhanced public + GitHub release (user approves) | H | S | |

Notes:
- Faithful ids: S1.1–S1.3 were S4.2–S4.4, S2.1–S2.2 were S4.5–S4.6, S3.1 was S4.7, S3.2 was S4.10,
  S4.1–S4.2 were S4.8–S4.9, S5.1 was S4.11, S5.3 was S4.12. S5.2 is new (the other Enhanced repos have
  a launcher).
- Every phase: `--classic` and the emulator build stay byte-identical to the faithful port;
  `python tools/verify_campaign.py` must stay OK after any change to the natives.
- S0.1: `original/` is a junction to `../Flight Simulator 1/original` (disk images, ignored). Exe
  `fs1-enhanced` (CMake target still `fs1`, `OUTPUT_NAME`); `tools/trace_campaign.py` and
  `tools/package.py` use the new name. Verified: both builds clean, `verify_campaign.py` OK,
  `--classic --frames 120` boots headless.
- S1.1: design in `docs/RENDER.md`, skeleton `src/enhanced/displaylist.{h,c}` (format, hook API, no
  hooks placed). Key points for S1.2/S1.3: projection hooks only *stage* floats, drawing hooks *emit*
  (matched on the integer endpoints); horizon floats go through a capture shadow keyed by the capture
  address; sky/ground sides are read back from the back buffer; polygons are outline + flood fill in
  the original (3.4.md), recorded as a reserved `DL_POLY` under its edges; anything unrecorded shows
  through a per-tile CGA fallback (owner/expect shadow of B800), so a missing hook is blurry, never
  wrong. `--dl-check` (S1.2) proves the hook list complete.

- S1.2: recorder and hooks in `displaylist.c`; `--dump-dl FILE`, `--dl-check` ("every back-buffer write was
  recorded" in a flight run; the polygon fill-vs-outline report lists 12 of 36 polys, e.g. a 82 vs 69 px box).
  Follow-up (2nd pass, same day): outline edges are joined into rings in either direction and clipped chains
  are joined along the view border; a polygon whose flood fill leaked out of its outline (mountain ridges:
  the fill runs down to the horizon) gets `DL_F_LEAK` plus the box `x0,y0-x1,y1` of the changed pixels in
  `DlItem.u.poly` (see docs/RENDER.md, "S1.2 as built"). **S1.3 should show that box from the CGA picture**
  (not yet wired in `sharp.c`). All 14 campaign sessions are `--dl-check` clean (except `reboot`: boot text).
- S1.3: `src/enhanced/sharp.{c,h}`. The CGA texture is drawn as before, then the committed view list is
  drawn over the view rows as one `SDL_RenderGeometry` batch (fills, sky/ground split by the horizon line,
  ear-clipped polygons, lines as quads of `--line-width N` output pixels (default 2; 0 = `SDL_RenderLine`
  hairlines), dots) clipped to the view. Colours: a pattern decoded through the composite NTSC LUT
  (`pc_composite_rgb`) and averaged, +20% saturation (D1). Per scan-line tiles of 16 px whose B800 bytes no
  longer equal the blit snapshot (`dl_view_snap`) are redrawn from the CGA texture. F11: sharp → composite →
  RGB (`--classic` keeps composite/RGB). Sharp needs the composite monitor and CGA mode 1A. `--rgb`,
  `--classic`, `--verify`, `--native-off` start with sharp off (recording off). Screenshots (`--screenshot`)
  in sharp mode are the window picture. Verified: `verify_campaign.py` OK, flight run cycles identical to
  `--classic` (56353233), `--dl-check` clean. Exe locked while the game runs: build to another dir.
- S2.1: needles only, by reading state (`docs/RENDER.md` 6b); no engine hook besides `pc_render_cga_vram` (pc.c)
  and `sharp_render_cga` (main.c). Five needles found in ~all flight frames of a 4500-frame take-off run;
  `--classic` screenshot and cycle count unchanged. Fitted lines checked numerically against the sprite pixels
  (`--dump-panel`), not by eye. Indicators / altimeter band / turn indicator stay crisp pixels (they are small
  sliding sprites; vector tracing gains nothing). The artificial horizon is split off as S2.3.
- S2.2: text by one hook (`textdraw.h`: `dl_text_row`, ENH) that records each glyph row word the print_str family writes to
  B800 (`displaylist.c`, retained per video RAM offset). `sharp_render_cga` keeps a row only while video RAM still holds
  exactly those bytes (anything overwritten, cleared or restored drops out), zeroes the rows in the CGA copy (the original
  writes whole words, so the background under a glyph is black) and fills a grid of fat-pixel cells (nibbles); `sharp_view`
  draws each cell as a rectangle with its convex corners cut and bridges diagonal-only neighbours (colour = composite
  decode of the nibble), after the view and the fallback tiles. Works on every screen in 640x200 composite mode (start-up
  menus, panel text, editor), not only in flight. Verified: `verify_campaign.py` OK, 1500-frame run cycles identical to
  `--classic` (119317525), ~900 text rows seen on the start-up screen. Shifted text (print_str2) is cell-quantised to
  whole nibbles. Not checked by eye (U1).
  Follow-ups (same session, after the first U1 build): `text_detect` also finds buffered/blitted text (flight status,
  radio) by matching the game's glyphs in video RAM; glyphs found this way are drawn with the 8x8 text font (`pc->font`),
  white (grey when dim), one rectangle per lit run; unclaimed rows (shifted text, panel art) keep the smoothed cells
  (strokes on the hi-res pixel grid, no composite colour fringes). Details in `docs/RENDER.md`. U1 should judge this
  version: the letters now look like the 8x8 font, not the game's thin 16x5 shapes (a deliberate change).
- S2.4: panel labels are not the game font (a 5-line dotted-stroke font baked into the panel bitmap), so they are traced, not
  identified: `label_scan` (`sharp.c`) finds clusters of lit pixels 5 lines tall and <=13 wide in the clean panel (cseg:0122,
  rescanned when its hash changes; 131 clusters / 2115 pixels in the stock panel), bridges 1-pixel gaps in a stroke, and
  `label_apply` (flight only) clears them from the CGA copy while the video RAM still holds all their pixels; the back end
  draws them on the hi-res pixel grid as smoothed white strokes (`draw_grid`). Cycles identical to `--classic` (73019456 at
  frame 910). Not done: the broken panel values from the first U1 row (S2.2 bug: `bc`, `f g`, `yyy`) -- separate fix.
  Follow-up 2026-10-06 (after the first U1 look): the broken panel values (`bc`, `f g`, `l`, `yyy`) were game codes above 0x5F
  (panel symbols) matched to the ROM font's lowercase letters; `font_load` now detects only 0x21-0x5F, the symbols keep the
  smoothed-cell look. Label clusters may be up to 24 wide (merged letters). The first U1 screenshot still showed fringed
  labels: rebuild first, then re-check.
- S2.5: `src/enhanced/panel/` (`cockpit.{h,c}` registry/blanking/frame state, `kit.{h,c}` vector kit + 8x8 panel font), `docs/PANEL.md`
  (flow, API, measured layout of the 16 modules, how to write one). Hooks only in `sharp.c` (`cockpit_begin/owns/add_*/end/draw`);
  `pattern_colour` moved to `kit_composite`. `--module-off NAME|all`, `--module-outline`. No module drawn yet (all `draw` NULL), so the
  picture is unchanged. Verified with a throwaway airspeed module: rect blanked (coloured pixels 3208 -> 123), needle 0x92F and 20
  text hits delivered, `--module-off airspeed` restores the old look; cycles identical to `--classic` (73019456 at frame 910).
- S2.6: `src/enhanced/panel/airspeed.{h,c}`. Dial measured from the clean panel (ring x 5..105, lines 123..167, centre 55,145; labels 40..200
  every 20 kt, 40 at the upper right, clockwise, 120 at the bottom). Needle angle is the game's own: `ias = airspeed(DS:08A8) * 28Fh >> 16`
  clamped 33h..100h, angle = (ias - 25) * 360/256 (checked against the S2.1 fitted needles at 40..200 kt: within ~3 deg, the fit's error);
  ias = 1.28 * knots, so the ticks sit at the exact needle angles. Rect shrunk to y 121..168 so the glareshield arc (lines 108-118) stays.
  Everything drawn: face, bezel, 10-kt ticks, numerals, KNOTS, needle with tail and hub. Cycles identical to `--classic` (73093945 at frame 910, keys 120:A,180:B).
- S2.7: `src/enhanced/panel/attitude.{h,c}`, `kit_fill_poly` added to the kit. The horizon is the game's own chord model, read from DS (view_bank hi byte,
  view_pitch, table 1CCB; formula in the file header), with the fraction kept for a sub-pixel line; checked against the game's row records
  (DS:1BAB) for two bank/pitch combinations (columns within 1-2 units of 4 px). Sky/ground colours = the game's pattern pair at DS:1CEB. Own design: ring with fixed
  roll ticks (10 deg to 60) and a zero index, disc, pointer that turns with the disc, fixed aircraft symbol and short pitch marks (the original's
  XOR bank dots and symbol are replaced). Cycles identical to `--classic` (73093945 at frame 910). Not checked by eye (U1): the pointer direction
  (bank right -> pointer left of top) follows the instrument convention, not verified against the game's dots.
- S2.8: `src/enhanced/panel/altimeter.{h,c}`. Both hands from the game state: A = (int16)(altitude word DS:30D6 + ground elev 0906 + offset 1E20) * 6B8Fh / 8000h
  (1/256 turn); long hand = A mod 256, short hand = A mod 2550 / 10 (1000 / 10000 ft per turn). Checked against the S2.1 fitted needles with a poked altitude
  (long hand 23.3 deg vs 21.7 fitted; the short hand's sprite is a wedge, the fit is rough, the formula is the game's own). The old dial's small boxed
  label (x 268..288, lines 130..135, 4 dotted glyphs, static art) is not redrawn: its meaning is unknown (no state drives it). Cycles identical to `--classic`.
- S2.9: `src/enhanced/panel/omi.{h,c}`. All from the game state as `obi_display` shows it: course needle DS:1A10 (0..60, x = 340 + n, one pixel per step, drawn as a
  line clipped to the card circle), glide-slope bar DS:1A12 (line 122 + n), flag DS:1A14 (0 OFF, 1 TO, 2 FROM; the original prints panel symbols, ours is text),
  marker lights DS:1A16 (bit 0 outer, 1 middle, 2 inner; left to right O M I, blue / amber / white), OBS course and reciprocal = the ASCII digits of the readout
  record DS:07B9 (+2, +8). The lights sit in this gauge's rect (lines 115-119), not in the COMPASS box: S2.15 does not need them. The needle and bar are drawn
  also when the flag is OFF (the original does too). Own layout: 4+4 deviation dots at +-11/17/23/29 px, centre box. Cycles identical to `--classic`.
- S2.10-S2.13 (bottom row): `turn.c`, `heading.c`, `vsi.c`, `tach.c`, `strips.{h,c}` (shared rudder track/marker, drawn by whichever of heading/vsi holds the marker).
  All needles/values from the game state: turn tilt = turn_rate (085A) * 190h >> 16 clamped +-6, 7.3 deg per step (measured from the 13 sprites at DS:16DD), ball =
  byte DS:08F7 (x = 24 + frame + 3, frame = ball + 31); heading = the 3 ASCII digits at DS:0799 (the card is an addition, the original shows only digits + symbol);
  VSI = oldest sample DS:091B * 7D0h >> 15 (+-125 units, 0 at 9 o'clock), numerals 5/10/15 at 45 degrees; tach = engine_rpm (1DC0) * 3B6h >> 8 + 8Ah, numerals 5..35
  placed 270 -> 502 degrees (read from the clean panel: 5, 10, 15 ... 35; the needle reads 6 at idle, 34 at full power). Control indicators are
  draw_indicator descriptors (12 bytes, DS:1401..: ball 1431, aileron 143D, rudder 1449, elevator 1455, throttle 1461, flaps 146D, trim 1479; +6 = frame):
  elevator marker y = 187 - frame, rudder marker x = 176 + frame + 3.5 on the dotted track at line 199 (x 186..239). The aileron track/marker (line ~159-160, x 176..248)
  lies above the modules and is still CGA pixels (S2.19). Cycles identical to `--classic`.
- S2.14-S2.18 (`right.{h,c}`): all values from the game state. Slider markers are draw_indicator descriptors (frame at +6): flaps 146D (y = 126 - f), trim 1479 (y = 153 - f),
  throttle 1461 (y = 199 - f) at x ~435; FUEL/OIL bars 1401, 140D, 1419, 1425 (x = 528 or 584 + f, one pixel per frame). Texts are the ASCII records the game prints
  (header word + characters): compass digits DS:079F, COM 07A5/07AB, NAV 07B0/07B6, transponder 07C7 (digits at every 2nd char), time 07D1/07D6/07DB. The O/M/I lights
  are in the COMPASS box (DS:1A16 bit 0 outer .. 2 inner), not in the OMI gauge: the OMI gauge's top digits are the OBS course, the bottom ones its reciprocal (fixed in S2.9).
  Status words: gear DS:057E (DN/UP), lights 0582 (ON/OFF), mags 0583 (OFF, L, R, BOTH, ST), carb heat 0577; the original prints panel-symbol words there,
  ours is plain text (the original shows just a "B" for mags 3 at the start). The strip letters F, U, D, T and the labels were decoded from the clean panel by eye.
- S2.19 (`frame.c`, module with `rest` = true): owns everything from line 106 down outside the other modules' rects. Built from the clean image whenever the owned pixels
  change (the clean image also changes inside modules): runs of 12+ pixels (a gap of 1 allowed) become lines (dotted = grey), vertical runs of 8+, the rest crisp
  pixel runs. Also draws the aileron marker (DS:143D, x = 176 + f + 3.5, track line 160). `cockpit_in_module` (bitmap) added to cockpit.c; blanking and ownership skip
  the other modules' rects, so `--module-off NAME` still shows that module's old look inside the frame. Rows 0-105 of the clean image are a solid placeholder for the view: not touched.
  Cycles identical to `--classic` (73093945 at frame 910).
- Follow-ups after the first panel look (2026-10-06): (a) `sharp_render_cga` also treats the start-up menu screens as panel screens: the panel is recognised by its two vertical lines
  (x 425/427) in the video RAM, a snapshot of the video RAM then serves as the image the frame module traces (the clean image at cseg:0 is not loaded yet in the menus); (b) the
  S2.2 smoothed text cells skip owned areas (the old status symbols no longer show under the new words); (c) `cockpit_gap_bottom(x)` (first frame line of the panel in column x,
  from the clean image, smallest of x+-2) lets `view_draw` continue the sky/ground fill below the view border down to the frame: `sky_rect` = the old split with a rectangle.
  Only fills continue (the engine clips the scenery lines and polygons at the view border; showing more of the world there would need a taller projection = engine change).
- S3.2: per-point interpolation instead of a camera pose (the scenery changes eye position, origin and projection many times per frame): every 3D point is a
  vertex (float eye vector from the raw point, identity = scenery address + step + occurrence), matched to the previous committed frame at the commit; the back
  end re-projects lines, dots, the horizon and polygon outlines (rings rebuilt) at the display time. Five one-line hooks (`transform.c`: world_to_eye_delta,
  rotate_point; `scenery.c`: clip_project_line entry, dotted/dashed set-up and step), design in `docs/RENDER.md` 6d. `--smooth on|ahead|off` (default on =
  one game frame late), launcher "Motion". Measured: the game draws a view every 181-266 ms; 96.7-100% of the vertices are found in the previous frame in every
  campaign session; drawn at their own time the frames match the recorded floats (max 0.5 px except a few long ground-polygon edges behind the eye, up to 17 px:
  the game's fixed-point clip). `--dl-check` clean as before (release build; the emulator build's unrecorded-write lines are the same at HEAD). Cycles identical
  to `--classic` in every mode (73093945 at frame 910), `--classic` screenshot identical, `verify_campaign.py` OK. Not checked by eye (U1).
  Fix after the first U1 ("no change"): the interpolation was timed by the CPU cycle counter, which jumps by the whole scenery pass (~0.9 M cycles, one native
  step) and then stands still while the display pays it back; it is now timed by the display clock (`target_cycles`, passed to `sharp_view`) from the first
  display frame that shows a view: s grows by ~0.075 per display frame, continuous across views.
  2nd U1 (2026-10-07): smooth OK, `--smooth ahead` bad (removed; launcher: a "Smooth motion" checkbox); jaggy shore and flicker: a water polygon of which only the
  shoreline crosses the view was closed by a chord, flagged leaking and drawn as a CGA box (69 of 436 demo frames). Outlines now keep the scenery's direction and are
  closed along the view border on the side the game filled (`stitch_rings`, winding from the changed pixels, kept for the re-projection): 1 leaking polygon left in the
  demo; `--dl-check` clean in every session; polygons rebuilt at their own time match (0 off). Cycles identical to `--classic`.
  3rd U1 (2026-10-07): looks good; on a long flight along the coast the shore line stands still and then slides to a new place. Cause in the game: op 24 local
  frames truncate the eye position to 2^s units (s up to 16), so far scenery jumps a step now and then (the original too; smoothing made it a slide). `dl_origin`
  (one hook in `h_origin`) keeps the fraction and the vertices use eye + fraction (RENDER.md 6d). Cycles identical to `--classic`, `verify_campaign.py` OK.

## User tasks
### U1 — Visual checks (when an agent asks)
Run the build, or open the screenshot the agent names, next to the authentic mode (`--classic`), and
reply in one line: `OK`, or what is wrong and where (e.g. "runway lines 2 px too far left", "panel
needle stays at zero"). Agents do not look at images themselves.

### U2 — Playtests and bug reports
```
Build: <commit hash>   Mode: authentic / sharp, aspect, other options
Steps: <what you did, from launch>
Classic does: <expected>
Enhanced does: <what happened; crash message if any>
Files: work\bugs\<n>.png
```
Several bugs in one message; one session then fixes them in a batch.

### U3 — Decisions: answered 2026-10-03, see Decisions D1–D4

## Decisions
- 2026-10-03: separate Enhanced repo (faithful U6.1 answered by the workflow: faithful first, Enhanced
  after its release).
- 2026-10-03: GitHub repo https://github.com/kylofon/fs1-enhanced created, private until the first release.

- 2026-10-03 (U3 answers):
  - D1 Palette: richer than the four composite colours, but only enough for minor adjustments and clarity. Nothing fancy.
  - D2 Enhanced is standalone: it never launches as the original. Default and only user-facing mode is sharp;
    the launcher has no "classic" option. `--classic` stays as a developer/verification flag only
    (required by the byte-identical gate), undocumented for players.
  - D3 No widescreen (S3.1 dropped); 4:3 kept, scaled to the window.
  - D4 First release is v0.1.0.
  - The game needs a launcher for its options, like the other ports (S5.2).

## TODO (parked)
- [ ] Linux/macOS builds and CI.
