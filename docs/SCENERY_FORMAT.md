# Scenery format: MS Flight Simulator 1.05 (IBM PC)

The scenery is not a table of objects but a **bytecode program**. `scenery_interp` (0050:3CC0) runs it
once per frame from `draw_scenery`. The same interpreter, in capture mode, runs the horizon programs
for `horizon_fill`. The program decides for itself what to draw: it tests the aircraft position,
time of day and other variables, and branches over objects that are too far away or not facing the eye.
It also tunes the radios, sets the airport elevation, ATIS texts and the demo flight, and can set
the crash code.

`tools/scenery_dump.py` disassembles all of it from the user's disk image and memory dump (see the end of
this file). All addresses are `DS:offset` (DS = 0618) unless they are code (`0050:offset`). Opcode,
handler and variable names are the ones proposed in [subphases/3.7.md](subphases/3.7.md).

## Storage and loading

| Program | Where | Loaded by |
|---|---|---|
| Area 0 Chicago (Meigs, O'Hare, Midway, Champaign...) | track 0F, 8441 bytes | `select_scenery_area` |
| Area 1 Los Angeles | track 12, 8208 bytes | " |
| Area 2 Seattle | track 15, 8319 bytes | " |
| Area 3 New York / Boston | track 18, 7250 bytes | " |
| Area 4 WWI (war mode area, no radio data) | track 1A, 3429 bytes | " |
| Horizon, 8 programs (one per heading octant) | DS:1D5F-1DB6, pointer table DS:1D4F | part of the loader image |

`area_tracks` (DS:03B4) holds the start tracks; the boot stream writes it with its `04` records.
`area_bounds` (DS:0449) holds 8 bytes per area: north min, north max, east min, east max (integer position
words, signed), ending with FFFF. `select_scenery_area` (0050:03CA, every 4th frame) takes the first
rectangle that contains `pos_north.hi`/`pos_east.hi`. If it is a new area, it does this:

1. Clears the demo-script pointers `[0571]` and `[31E8]`, and sets `[3628]` = 1 (ATIS idle).
2. Sets `disk_track`, and sets `[03B0]` = 0FFF so that the next `boot_stream_byte` reads the track.
3. Reads one boot-stream record: a type byte (always 01), a word length L, then L-3 bytes into
   `DS:[scenery_base]` (3A84). Tracks are 8 × 512 bytes. The record runs on into the following tracks,
   exactly as in the boot loader (`tools/pc_loadstream.py`).

All five areas end on a `19` (return) byte. The program is entered at its first byte.

## Interpreter

```
scenery_interp:                     ; 0050:3CC0
  DS = [0000:0120]
  [31D4] = [31D3]; [31DF] = [31DE]  ; one-shot NAV / COM search flags for this pass
  loop:
    bx = scenery_ip; al = [bx]
    if al == 79h: break
    if al > 42h: hang (jmp $ at 0050:45B1; the bytes after it read "BADOP")
    call [3204 + 2*al]              ; handler gets BX = address of the opcode byte
    DS = [0000:0120]
  if capture_mode: byte [[30C6]] = FFh   ; terminates the captured list
  [31DF] = [31D4] = 0
```

Each handler advances `scenery_ip` itself, so an opcode's length is whatever its handler adds. Every
branch offset is relative to the address of the branch's own opcode byte. The handlers clobber all
registers. Nothing is kept on the 8086 stack between opcodes, so all interpreter state is in DS variables.

## Coordinate system and units

- **World axes.** x = east, y = up, z = north. This is the order of `eye_pos` (30EB, 30ED, 30EF) and of
  every point operand.
- **Position.** `pos_east` (30D1) and `pos_north` (30D9) are 32-bit 16.16 values. The integer word
  (`.hi`, 30D3/30DB) is the unit the editor shows, minus 4000h. **One integer unit is 256 m.** Two checks
  confirm this:
  - Meigs' runway decodes to 4.70 units long, which is 1203 m (real: 3948 ft, or 1203 m).
  - Champaign is 770 units south of Meigs, which is 197 km (real: about 205 km).
- **Altitude.** `altitude` (30D5) is a 32-bit value with the word at 30D6 in metres. So the 32-bit value
  divided by 65536 is in 256 m units too, which makes the three axes isotropic.
  - The field elevations that the scenery stores in `[090A]` are metres: Meigs B4h = 180 m (592 ft),
    O'Hare CBh = 203 m (666 ft).
  - The altimeter adds `[0906]` (which follows `[090A]`).
- **Local frames.** Op `24` sets an origin O (three 32-bit values in the same 16.16 units) and a shift s.
  The handler then sets `eye_pos = (pos − O) >> s` for each axis and keeps only the low 16 bits; nothing
  saturates.
  - Point operands are signed 16-bit local coordinates, so world = O + (local << s).
  - One local unit is 2^s / 256 m:

    | s (operand) | Local unit | Typical use |
    |---|---|---|
    | 4 (02) | 1/16 m | runways, detailed airports |
    | 8 (04) | 1 m | buildings, mid-range level of detail |
    | 12 (06) | 16 m | far level of detail, coastlines, roads |
    | 16 (08) | 256 m | not used by any area |
    | 0 (00) | 1/256 m | not used by any area |

- **Points.** `world_to_eye_delta` (0050:48E8) computes point − eye_pos.
  - If a component overflows 16 bits, all three are halved four times, keeping the sign.
  - If any component is ≥ 2000h in size, it shifts right by 1 or 2.
  - If all are < 0400h, it shifts left by 3 until one is not.

  This is a uniform scale, so the perspective divide doesn't change; only precision does. The delta is
  then rotated by `view_matrix` (`rotate_point`) into eye space, and given an outcode.
- **Flat points.** Ops `40`/`41` (and the horizon) take only x and z. y comes from `[315B]`, which nothing
  ever writes (it is 0). So flat points lie at the frame origin's height, which is the ground.
- **Before the first origin.** Points use whatever `eye_pos` holds from the previous frame. Every area sets
  an origin before its first point.

## Colours and patterns

The back buffer has 160 × 106 pixels, one nibble each. `draw_colour` (30C8) is a word:
- the **low byte** is ORed into odd-x pixels (low nibble)
- the **high byte** is ORed into even-x pixels (high nibble)

So `F00F` draws solid colour F, and `0002` draws colour 2 on every other pixel only, as a dotted line.
On a composite monitor these nibbles are artifact colours.

| Op | Effect |
|---|---|
| `12 c` | `draw_colour` = (c << 12) \| c, which is solid colour c (for c ≤ F). `[3159]` = (c \| c<<4) in both bytes. The value 11h occurs once and mixes bits into the other nibble |
| `0C w` | `draw_colour` = w only at dusk (`time_of_day` & 2) |
| `2E w` | `draw_colour` = w only at night (`time_of_day` & 4) |
| `1B` | `draw_colour` = 5005h (solid colour 5) at dusk or night |
| `1C` | `draw_colour` = 0002h (dotted colour 2) at dusk or night |

By day, `12` sets the colour and `1B`/`1C` do nothing. At dusk or night, `1B` and `1C` switch each edge
between "lit" (solid 5) and "dim" (dotted 2). That is how runways and shorelines turn into rows of lights.

`time_of_day` (0412) is set by `sub_2FD0` from `clock_hour:clock_min` against a per-season table at
DS:1E2C: 1 = day, 2 = dusk, 4 = night.

**`[31D0]`: polygon mode.**
- `2F` sets it to 1 and clears the edge list at DS:3730. While it is set, `draw_line` takes its alternate
  path at 0050:5060. This path draws nothing; it records the edge's per-row byte addresses in the
  4-byte entries at DS:3734 (bit 15 = which nibble).
- `2D` sets it back to 0 and terminates the list. Then, **by day only** and not in radar view, it fills
  the spans between paired edge addresses (`sub_549B`/`55B7`). The fill uses the pattern in
  `fill_pattern` (31CE).
- The scenery sets that pattern with `25`/`1A`, for example `copy [31CE] = ground_pattern` for lakes, or 2222h.
- At dusk and night, polygons draw nothing.

**Sky and ground.** Each area begins with a weather block, which does the following:
- compares `[0900]` (the altimeter value, written by `upd_altimeter`) with the editor's cloud-layer words
  (`[206B]`, `[206D]`, `[206F]`, `[2071]`)
- copies the right pair from the display-type colour table at DS:31EA-31FA (filled in by `startup_menus`)
  into `sky_pattern` (1D48) and `ground_pattern` (1D4A)
- also sets `[0416]` (2222/5555/FFFF), which `view_overlay_marks` uses as a fill byte (?)

## Opcode reference

"Traced" means the handler's first byte has flag 0x01 in `extracted/trace.bin` (linear 0x500 + handler).
The 14-session campaign only runs in area 0, so opcodes that only other areas use can show "no".

Operand notation:
- `b` byte, `w` word, `s` signed word
- `r` rel16 branch (from the opcode byte)
- `v` DS address of a word variable
- `x y z` a point: three signed words in the order east, up, north

| Op | Name | Bytes | Operands | Handler | Traced | Effect |
|---|---|---|---|---|---|---|
| 00 | dot | 7 | x y z | 3D17 `op_dot` | yes | Projects the point and plots one pixel (`project_dot`: z ≤ 1 or \|x\|,\|y\| ≥ z is dropped). In capture mode it appends FE,x,y instead |
| 01 | move | 7 | x y z | 3D8E `op_move_to` | yes | Sets the pen to the point (eye space at 310F). Also saves it as the polygon start (3133) for `29` |
| 02 | line | 7 | x y z | 3DB3 `op_line_to` | yes | Line from the pen (or from the last line end if no move is pending) to the point: clip, project, `draw_line` |
| 03 | – | – | | 3DDB (`ret`) | no | Doesn't advance the IP, so it hangs. Invalid |
| 04 07 09 0A 13 14 16 1F 26 27 2C 30 36–3F | – | – | | 45B1 | no | Hang (`jmp $`). Invalid |
| 05 | viewpoint | 13 | x y z, w pitch, w bank, w heading | 3E1A `op_set_viewpoint` | no | Sets `eye_pos` and the view angles, then `build_view_matrix` |
| 06 | line2d | 5 | b x0, b y0, b x1, b y1 | 3E47 | no | `draw_line` in screen pixels, unclipped |
| 08 | clear_view | 1 | | 3E69 | no | `clear_view_buffer` |
| 0B | jump | 3 | r | 3E70 `op_jump_rel` | yes | IP += r |
| 0C | colour_dusk | 3 | w | 3EEC | yes | See Colours |
| 0D | capture | 3 | w addr | 3E78 | no | addr ≠ 0: `capture_mode` = FF, capture pointer `[30C6]` = addr. addr = 0: capture off |
| 0E | proj_scale | 5 | w, w | 3E98 | no | `proj_params` x/y scale bytes (3167/3168) = w1; centre x/y (3169/316A) = w2 |
| 0F | – | – | | 3EAE (`ret`) | no | Hangs, like 03 |
| 10 | cga_regs | 1 | | 3EAF | no | `cga_program_regs` with its default table 399C |
| 11 | nop | 1 | | 3EB6 | no | |
| 12 | colour | 2 | b c | 3EBB | yes | See Colours |
| 15 | draw_captured | 1 | | 408C `draw_horizon_list` | yes | `draw_line_list` on `horizon_list` (041F): x0,y0,x1,y1 byte records, FF ends |
| 17 | demo_script | 3 + n | r | 45A2 | yes | `[31E8]` = address after the operand; IP += r. The skipped bytes are the area's demo key script |
| 18 | call | 3 | r | 4517 | yes | `[31D1]` = IP+3; IP += r. One level only: there is no stack |
| 19 | return | 1 | | 4523 | yes | IP = `[31D1]` |
| 1A | copy | 5 | v dst, v src | 4592 | yes | word [dst] = word [src] |
| 1B | colour_dark_a | 1 | | 3EF4 | yes | See Colours (5005h) |
| 1C | colour_dark_b | 1 | | 3F07 | yes | See Colours (0002h) |
| 1D | nav_station | 11 | w BCD freq, 4-byte east, 4-byte north | 452A | yes (match path no) | NAV station, see Radio |
| 1E | com_station | r | r length, w BCD freq, 4 b runways, 4 b temps, ATIS text | 4556 | yes (match path no) | COM/ATIS station, see Radio. IP += r |
| 20 | if_in | 9 | r, v, s lo, s hi | 3F1A | yes | If lo ≤ [v] ≤ hi (signed), IP += 9; otherwise IP += r |
| 21 | if_in (2) | 15 | r, (v lo hi) ×2 | 3F3B | yes | Both ranges must hold |
| 22 | if_in (3) | 21 | r, (v lo hi) ×3 | 3F67 | yes | All three must hold |
| 23 | if_bits | 7 | r, v, w mask | 3FA6 | yes | If [v] & mask ≠ 0, IP += 7; otherwise IP += r |
| 24 | origin | 14 | b shift selector (0/2/4/6/8), 4-byte east, 4-byte up, 4-byte north | 3FF0 | yes | Sets the local frame. See Coordinates. The shift goes through table DS:316C (4073 none, 4063 >>4, 4074 >>8, 4079 >>12, 4089 >>16) |
| 25 | store | 5 | v, w | 3F0C | yes | word [v] = w |
| 28 | jump_if | 8 | b cond, r, v a, v b | 3FBE | yes | Compares [a] with [b], signed. cond 0 is ==, 2 is >, 4 is < (table DS:3176). True: IP += r; false: IP += 8 |
| 29 | close | 1 | | 3DDC | yes | Line from the last point back to the last `01`/`40` start |
| 2A | dotted | 14 | x y z (A), x y z (B), b n | 43A1 | no | n dots from B towards A, evenly spaced |
| 2B | dashed | 14 | x y z (A), x y z (B), b n | 44BF | yes | B→A split into n steps; draws every other step (n/2 dashes). Used for runway centre lines |
| 2D | poly_end | 1 | | 5468 | yes | Ends the polygon. By day it fills it. `[31D0]` = 0 |
| 2E | colour_night | 3 | w | 3ED8 | yes | See Colours |
| 2F | poly_begin | 1 | | 44FB | yes | Starts a polygon. `[31D0]` = 1 |
| 31 | cache_point | 9 | b slot, x y z | 4140 | yes | Transforms the point once and stores eye x,y,z plus the outcode in slot DS:3288 + 8×slot. The areas use slots 0–20 |
| 32 | move_cached | 2 | b slot | 4178 | yes | `move` to a cached point |
| 33 | line_cached | 2 | b slot | 418E | yes | `line` to a cached point |
| 34 | skip1 | 2 | b | 41CB | no | IP += 2 (no effect) |
| 35 | dot_cached | 2 | b slot | 41D1 | yes | `dot` at a cached point |
| 40 | move_flat | 5 | x z | 3D8A | yes | `move` with y = `[315B]` (0). Sets `[315D]`, which `world_to_eye_delta` reads |
| 41 | line_flat | 5 | x z | 3DAF | yes | `line` with y = `[315B]` |
| 42 | – | – | | [3288] | no | Off the end of the table: the word there is point-cache slot 0. Invalid |
| 79 | end | 1 | | (interpreter) | | Stops the program |

**Which opcodes are used.**
- The five areas and the horizon use only these: 00 01 02 0B 0C 12 17 18 19 1A 1B 1C 1D 1E 20 21 22 23
  24 25 28 29 2B 2D 2E 2F 31 32 33 35 40 41 79. All of their handlers ran in the traces.
- 05, 06, 08, 0D, 0E, 10, 11, 15, 2A and 34 are valid but appear in no program. The code still runs 15's
  handler, `draw_horizon_list`, directly from `draw_sky_ground`.

**Line state.**
- `[3163]` is set by the move ops: "the next line starts at the pen".
- `clip_project_line` clears it, so the next `line` continues from the previous line's end (a polyline).
- `[315E]`, `[315F]`, `[3160]` and `[3161]` are the outcodes of p1, p2, the last end and the start.
- Clipping (Cohen-Sutherland against x = ±z and y = ±z) and projection are the existing `clip_project_line`
  and `project_dot`.

## Visibility, level of detail, conditions

No opcode measures distance. The program does all culling with the range tests `20`/`21`/`22` and the
bit test `23`:

- **Level of detail.** One block of tests guards each object at each level of detail. The tests are on
  `pos_north.hi`, `pos_east.hi` and `altitude.hi`, in 256 m units.
  - For example, Meigs at E 287, N 786:
    - within ±7 units (about 1.8 km) and below 7 units of altitude, the >>4 version is drawn
      (runway outline, dashed centre line, markings)
    - otherwise, within ±100 units, the >>8 version (a few lines)
    - from further away, the >>12 version
  - Area 0 contains one dead block: a disabled O'Hare middle level, jumped over at 51F8.
- **Facing and hidden lines.** After an origin, tests on `eye_pos` and `[30EF]` (eye north) pick which faces
  of a building to draw. For example, `if_in [30EF] in [120, 32767]` draws the north face only when the
  eye is north of it.
- **Time of day.** `if_bits time_of_day & 0001/0003` (day, or day and dusk), plus the colour ops.
- **Radar view.** `if_bits radar_view & 0001` skips the weather/sky block.
- **Blinking.** `if_bits [041D] & 0003` or `& 0300`. `[041D]` is rotated left once per frame in
  `main_loop`, which makes beacons flash.
- **Editor values.** `jump_if [0900] > [2071]` and similar compare the altimeter with the cloud layers.

## Radio, ATIS and airport data

**Reception.** Each station is guarded by an `if_in` box on the position. NAV boxes are ±500 units
(128 km) around the station; COM boxes are ±300.

**NAV (`1D`).**
- Layout: word BCD frequency, then the station's east and north as 4-byte 16.16 values.
- The frequency word is the BCD of digits 2–3 in the high byte and digits 5–6 in the low byte, with the
  leading "1" implied. NAV 109.15 is stored as `15 09` (word 0915h).
- `[03FF]` holds the tuned value. `sub_1447` sets it, and `sub_147D` sets the search flag `[31D3]` and
  clears `[31D5]` (every 8th frame and on retune).
- In a searching pass (`[31D4]`), each `1D` whose frequency matches copies its 8 bytes to `[31D6..31DD]`
  and sets `[31D5]` = 1 (station received). The last match in the pass wins.
- `sub_2290` (OBI/DME) uses the record as east/north.

**ILS.**
- An ILS is a `1D` (its frequency ends in 5) followed by stores:
  - `[1A1F]` (localizer course; `sub_2290` uses it instead of the OBI course when `nav_freq+6` = '5')
  - `[1A1D]` (glide-slope parameter?)
- Two small boxes set `[1A19]` and `[1A1B]` = 1: the marker beacons. `sub_25D4` shifts them out into the
  marker lamps each frame.

**COM / ATIS (`1E`).**
- Layout: rel16 record length, then a word BCD frequency (the same coding as NAV, from `com_freq`), then:
  - 4 bytes: the runway in use for each wind quadrant. Byte = (wind heading `[2087]` >> 14).
  - 4 bytes: temperature in °F by season (`[206A]`−1), plus `[202A]`.
  - the ATIS text, up to the end of the record.
- `com_tune` sets `[03F2]`, `[31DE]` = 1 and `[3628]` = 1.
- On a match, the handler does the following:
  1. Copies the 8 bytes to `[31E0..31E7]`.
  2. Sets `[3626]` = the text address and `[3628]` = 0 (message running).
  3. Calls `atis_start` (0050:4FBF). This formats the variable fields: runway, temperature, the time in
     Zulu using `[1E24]` (UTC offset), wind, altimeter, ceiling from `[2071]`, and visibility.
- The text is streamed by `sub_4F7D`:
  - bytes 20h–7Fh are ASCII
  - `00` ends the message
  - `80h+k` inserts fragment k from the pointer table at DS:362D (14 entries). The fragments are the fixed
    ATIS phrases and the formatted fields; each ends with 00.
  - The airport name comes first ("MEIGS FIELD"), then the information letter.

**Airport data.**
- Just before each `1E`, a box around the airport stores the field elevation in metres in `[090A]`.
  `sub_0742` copies it to `[0906]` while the airspeed is below 0A00h.
- Area headers store the following:

  | Variable | Contents | Values |
  |---|---|---|
  | `[0915]`, `[0917]` | magnetic variation (binary angles) | 0AAAh = 15° for LA; 0FA4h = 22° for Seattle |
  | `[1E24]` | UTC offset in hours | 5, 7, 7 and 4 for Chicago, LA, Seattle and New York |

- War area 4 stores `[1E53]` (read by `war_bomb_fall`; a bomb-target score?).

**Demo script (`17`).**
- In demo mode, `sub_110C` runs from `int8_timer` at about 6 Hz, at the same rate as `flight_forces`.
  It feeds the bytes at `[31E8]` to `key_dispatch` as scancodes.
- `80h+n` waits n steps, and `7F` restarts the script.
- `select_scenery_area` resets the script pointer on an area change. The editor code at 0050:3B1C
  reloads it when not in demo mode.

## Crash detection

| Code | Message | Set by |
|---|---|---|
| 06 | BUILDING CRASH | The scenery itself: `store crash_code = 0006` inside an `if_in` on `eye_pos`, `[30ED]` and `[30EF]`, i.e. the eye inside the building's local box (area 0, 2 buildings) |
| 08 | SPLASH! | `draw_scenery` (0050:0467) after the interpreter runs: altitude (32-bit) < 400h (4 m) **and** the back-buffer word at 0C0C:3068 = 2222h. That word is row 105 (the bottom row), x 80–83 (the centre). So you splash when the ground just under the nose is drawn in the water pattern 2222h by a filled polygon |
| 02 | MOUNTAIN CRASH | Never written in 1.05, neither by code nor by any area |
| 04, 0A | hard landing, gear up | `flight_integrate`, not the scenery |

The scenery also sets `[0406]` = 1 (airport ramp) or 2 (war home field). When the aircraft is
stopped, `flight_params` (0050:1B84) then refuels, or (for 2) re-arms the guns and bombs.

## Horizon programs

- `horizon_fill` takes octant = (heading byte + 10h) >> 5 and uses the program at `[1D4F + 2*octant]`.
- It turns capture mode on, sets the capture pointer to `horizon_list` (041F), zeroes `eye_pos`, runs the
  interpreter, and then restores `eye_pos`.
- Each program is `40` (move_flat), `41` (line_flat), `79`. That is one line at eye height (y = 0):
  - octant 0: from (−4000, 1000) to (4000, 1000), i.e. 1000 units ahead and 4000 to each side
  - the other octants: the same line rotated, with corners at ±1000/±4000

  Its clipped screen endpoints become the sky/ground boundary.
- At dusk, `draw_sky_ground` also draws the captured line (`draw_horizon_list`) in colour 8008h.

## Tool

    python tools/scenery_dump.py                 # all five areas, from the .ima
    python tools/scenery_dump.py --area 0 --bytes --text
    python tools/scenery_dump.py --horizon --stats --check

The decoder follows control flow from the entry point: branches, calls, both sides of tests, and over
embedded data. It then decodes the bytes it didn't reach linearly, as dead code (marked `!`).

The result: **35647/35647 bytes in the five areas decode without desynchronising (100%)**, plus 88/88
bytes of horizon programs. This includes 232 bytes of dead code and 2852 bytes of embedded demo/ATIS data.

Four checks back this up:
- Area 0 is byte-identical to DS:3A84 in a runtime dump.
- Sampled `scenery_ip` values from runs of 457–1500 frames all land on decoded instruction starts.
  The `[31D1]` return addresses all follow a `call`.
- `--check` shows each area's geometry inside (or, for area 2, near) its `area_bounds` rectangle.
- In area 0, the geometry is around the start position E 287, N 786 (Meigs).
