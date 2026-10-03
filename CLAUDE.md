# FS1 Enhanced — agent rules (keep this file short: it is loaded every session)

Enhanced C/SDL3 port of Microsoft Flight Simulator 1.05 (IBM PC, 1982), built on the faithful port
`../Flight Simulator 1` at v0.1.0 (fdb9bf1). State and subtasks: `PLAN.md`. Engine rules: `ENGINE.md`.

Enhanced rules:
- Enhancements live in `src/enhanced/`. Every edit to engine code is marked `/* ENH: … */` and kept
  as small as a hook (`grep -rn "ENH:" src CMakeLists.txt` lists them all).
- `--classic` turns every enhancement off and must behave exactly as the faithful release: same memory,
  cycles and CGA picture. Recording for the new renderer is a side channel, never a change to the natives'
  results. Each enhancement is its own option; enhanced defaults are on.
- Any change to engine code (`src/natives`, `sched.c`, `cpu_core.c`) must keep
  `python tools/verify_campaign.py` OK (emulator build, `build-emu/`).
- The simulation is unchanged unless the subtask says so.
- A bug in the engine itself: fix it in `../Flight Simulator 1` first (its PLAN.md), then port it here.

Token budget rules:
- Work on ONE subtask per session (PLAN.md id, e.g. `S1.1`). Read only PLAN.md "Status" + that
  subtask. When done: status cell (`done YYYY-MM-DD`, `U1 pending`), `python tools/plan_site.py`,
  commit. Longer notes go in the "Notes:" list under the table, never between rows.
  When you need something from the user, add a row to `## User to-do` at the top of PLAN.md.
- Never read large files whole: `Grep` first, then `Read` with offset/limit. `src/game/state.h`,
  `docs/symbols.txt` and the big natives are searched, never dumped. Game state: `state.h` accessors
  (`python tools/gen_state.py` after editing symbols.txt).
- RE detail stays in `../Flight Simulator 1` (`docs/subphases/3.*.md`, `docs/PHASE3_PLAN.md`,
  `extracted/fs1.asm`): search it, never dump it, never re-derive what it says.
- Do not view images or screenshots unless asked. Visual checks are the user's job (U1): write the
  PNG / BMP, then ask the user what they see.
- Wide searches → the `scout` agent (Haiku).
- Windows: build with `C:\msys64\mingw64\bin` first on PATH. Edit files with the Edit/Write tools or
  Python only (PowerShell 5.1 corrupts UTF-8).
- Game data is never committed (`original/` is a junction to the faithful repo's ignored folder).
- Commits end with the attribution line from the session's instructions.
- No test HTML pages; implement, then tell the user what to test.
