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
| – | Nothing pending (U3 answered 2026-10-03) | – | – |

## Status

- **Done:** S0.1 (repo from the faithful port v0.1.0; `--classic`; verification gate OK).
- **Next:** S1.1 (Opus). No user tasks pending. Decisions D1–D4 below are binding.
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
| S1.1 | Design: display-list format and recording points in the natives (`raster.h` lines, `fill_rows`/horizon, `textdraw.h`, `panel.h` sprites/needles, scenery dots); sub-pixel coordinates from the projection before rounding; `docs/RENDER.md`; no code beyond a skeleton in `src/enhanced/` | O | M | |
| S1.2 | 3D view display list: record scenery lines/dots/polygons and horizon with float coordinates; `--dump-dl` to a text file per frame for checks | S | M | |
| S1.3 | SDL back end for the 3D view: lines (`SDL_RenderLines`, thickness option), filled sky/ground and polygons (`SDL_RenderGeometry`), slightly richer palette than the 4 composite colours (minor adjustments for clarity only, D1); toggle authentic/sharp (F11 cycles with composite/RGB); **U1 check** | S | M | |

### Phase 2 — Panel and text
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S2.1 | Instrument panel at high resolution: needles as lines/polygons, indicators and sprites upscaled or vector-traced from the original sprite data; **U1 check** | S | L | |
| S2.2 | Text at high resolution: the 16×5 game font rendered as scalable glyphs (vector from the font bits), panel and menu text; editor text mode with a sharp font | S | M | |

### Phase 3 — View
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S3.1 | ~~Widescreen~~ dropped (D3): keep 4:3, letterbox/pillarbox at any window size, integer or smooth scaling option | S | S | dropped 2026-10-03 |
| S3.2 | Smooth frame rate: run the simulation at the original rates, render the 3D view every display frame with interpolated eye position/attitude from the display list; no sim behaviour change | O | L | |

### Phase 4 — Input
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S4.1 | Gamepad and joystick (SDL3 gamepad API): analog elevator/aileron/rudder/throttle written into the control variables with the original key steps' scaling; dead zone; hot-plug | S | M | |
| S4.2 | Mouse yoke (optional, toggle key) and configurable key bindings (`fs1.ini`); in-game help overlay listing the keys | S | M | |

### Phase 5 — Settings, launcher, release
| Id | Task | Model | Size | Status |
|---|---|---|---|---|
| S5.1 | Settings and save state: `fs1.ini` (display mode, aspect, scale, audio, controls), F5/F9 quick save/load of the whole game state (memory array + registers) | S | M | |
| S5.2 | Launcher "FS1 Enhanced" (required, like the other ports) from `../StreetRodEnhanced/launcher/` (wxWidgets), adapted: disk image instead of a game folder, all options of S1–S5.1 (palette, scaling, smooth frame rate, gamepad, audio), key bindings; no "classic" entry (D2); built by default, `-DFS1_LAUNCHER=OFF` to skip | S | M | |
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
