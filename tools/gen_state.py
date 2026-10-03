"""Generate src/game/state.h (typed game-state accessors) from docs/symbols.txt.

usage: python tools/gen_state.py [--check]

Every `var 0618:XXXX name TYPE` symbol of the game data segment becomes:

  #define GS_NAME 0xXXXX                         offset constant (all symbols, any type)
  byte   -> uint8_t  gs_name(Pc *)  / void gs_set_name(Pc *, uint8_t)
  word   -> uint16_t or int16_t (see below)
  dword  -> uint32_t or int32_t (little endian, low word first)
  24-bit -> uint32_t (low 24 bits; the setter writes three bytes)
  table  -> gs_name_b(Pc *, i) / gs_name_w(Pc *, i)  byte / word at GS_NAME + i (+ 2*i)
            and gs_set_name_b / gs_set_name_w
  text   -> gs_name_ch(Pc *, i) / gs_set_name_ch    character i
  (none) -> the offset constant only

Signedness. A word or dword is signed when its symbols.txt comment contains the tag `[signed]`
or its name is in SIGNED below (controls, binary angles, rates, forces, sin/cos and other Q15
or two's-complement quantities, as the notes describe them). Everything else is unsigned.
Natives that need the other view cast the result; the bits are the same.

All accessors go through cpu_read8/16 and cpu_write8/16 at DS:offset (the live DS register,
normally GAME_DS = 0618, as the replaced instructions address it), so tracing and the
--verify write log see every access, and game memory stays the only copy of the state (the
original code still runs for the routines not replaced yet).

The header also documents the main state blocks (GROUPS below) with units and formats.
`--check` exits 1 if the committed header is out of date.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYMBOLS = os.path.join(ROOT, "docs", "symbols.txt")
OUT = os.path.join(ROOT, "src", "game", "state.h")

VAR = re.compile(r"^var\s+0618:([0-9A-Fa-f]{4})\s+(\w+)(?:\s+(byte|word|dword|table|text|24-bit))?\s*(?:;\s*(.*))?$")

# Signed quantities (two's complement). Binary angles (10000h = 360 deg) are signed too:
# -8000h..7FFFh is -180..+180 deg, and the add/subtract bits are the same either way.
SIGNED = {
    # controls (7FFFh = full deflection)
    "aileron", "elevator", "elevator_trim", "rudder", "slew_rate_a", "slew_rate_b",
    # angles
    "pitch", "bank", "heading", "angle_of_attack", "view_pitch", "view_bank", "view_heading",
    "view_pitch_trim", "touchdown_view", "flaps_view",
    # rates and accelerations
    "pitch_rate", "roll_rate", "turn_rate", "accel", "pitch_accel", "accel_to_pitch",
    "pitch_to_speed", "vertical_speed", "climb_step", "horiz_step", "east_step", "north_step",
    "wind_dx", "wind_dy", "level_rate_pos", "level_rate_neg",
    # forces and coefficients
    "drag", "lift", "thrust", "net_force", "normal_force", "weight_normal", "weight_along",
    "lift_horiz", "lift_left", "lift_right", "roll_moment", "cl_left", "cl_right", "cl_aoa",
    "cl_ofs_left", "cl_ofs_right", "flaps_cl", "aileron_cl", "elev_aoa", "slip_cl", "slip_yaw",
    "slip_drag", "cross_control", "crab_a", "crab_b", "gear_drag", "flaps_drag",
    # Q15 sin / cos
    "cos_pitch", "cos_bank", "cos2_bank", "sin_pitch", "sin_bank", "sin_track", "cos_track",
    # positions (16.16 / 24.8 fixed point)
    "pos_east", "pos_north", "altitude",
    # radio offsets
    "nav_dn", "nav_de",
}

# Main state blocks: (title, description, [symbol names]). Symbols not listed fall into a
# block by offset (see block_of).
GROUPS = [
    ("aircraft", """Position and attitude of the aircraft.
 *   pos_east, pos_north: dword, 16.16 fixed point; the integer word is in 256 m units
 *     (the editor's N/E values are the high words 30DB / 30D3).
 *   altitude: dword, byte 30D5 = fraction, word 30D6 = integer altitude; ground = 3.
 *   pitch, bank, heading: binary angles, 10000h = 360 deg (signed: 8000h = -180 deg).
 *   airspeed: V, knots = V * 259h / 10000h (about V / 109); ias_knots is the panel value.
 *   vertical_speed = V * sin(pitch); sin/cos terms are Q15 (7FFFh = 1.0).""",
     ["pos_east", "pos_north", "altitude", "pitch", "bank", "heading", "angle_of_attack",
      "airspeed", "airspeed_sq", "inv_airspeed", "vertical_speed", "on_ground", "on_ground_prev",
      "pitch_rate", "roll_rate", "turn_rate", "ground_elev", "field_elevation", "alt_display",
      "alt_m", "ias_knots", "crash_code", "stalled", "wing_damage"]),
    ("controls", """Pilot controls. Signed 16-bit, 7FFFh = full deflection; flaps 0 / 2000h /
 *   4000h / 6000h / 7FFFh; throttle 0..7FFFh (F2 full, F10 idle); byte switches 0/1.""",
     ["aileron", "elevator", "elevator_trim", "rudder", "flaps", "throttle", "throttle_target",
      "gear_down", "carb_heat", "lights", "magnetos", "auto_coordination", "slew_mode",
      "slew_rate_a", "slew_rate_b", "selected_item", "paused"]),
    ("flight model", """Terms of flight_forces / flight_integrate (3.13, 3.14). All 16-bit fixed
 *   point that wraps like the original's ADD/SUB; constants 08C0..08F4 are set at start-up.""",
     []),
    ("engine", """Engine and fuel (engine_update, 3.14). engine_rpm drives the IRQ0 rate;
 *   fuel_left / fuel_right are 24-bit, the gauge byte is the top byte.""",
     ["engine_rpm", "rpm_target", "engine_power", "oil_temp", "oil_press", "oil_temp_rate",
      "oil_press_rate", "engine_faults", "fuel_flow", "carb_ice", "rpm_deficit", "fuel_left",
      "fuel_right", "mag_switches", "mag_ok", "engine_sound", "engine_running", "fuel_weight",
      "rpm_by_power", "rpm_windmill"]),
    ("radios", """COM / NAV / transponder / OBI and ATIS (3.12). Frequencies are ASCII digit
 *   text as typed and BCD when tuned (124.85 -> 2485h); obi_course in degrees; nav_dn / nav_de
 *   offsets from the station in 16 m units; the needles are pixel positions.""",
     ["com_freq", "nav_freq", "transponder", "com_bcd", "nav_bcd", "obi_course", "nav_search",
      "nav_found", "nav_station", "com_search", "com_station", "nav_dn", "nav_de", "nav_flag",
      "nav_flag_drawn", "loc_needle", "loc_target", "gs_needle", "gs_target", "marker_a",
      "marker_b", "marker_inner", "marker_state", "ils_param", "ils_course", "atis_ptr",
      "atis_idle", "atis_fragments", "atis_seg", "atis_lines", "atis_ring_a", "atis_ring_b",
      "atis_idle_count", "atis_in_fragment", "atis_frag_ptr", "atan_table", "obi_needle_recs"]),
    ("panel", """Instrument panel (3.10, 3.11): panel_mask selects which gauges update this
 *   frame (frame_counter & 3 is the panel phase); needle descriptors and sprites.""",
     []),
    ("war", """Europe 1917 (3.17): enemy_table is 6 records of 29 bytes; bombs_left is an
 *   ASCII digit.""",
     ["war_mode", "war_active", "war_status_line", "ammo", "bombs_left", "bomb_state",
      "bomb_whistle", "bomb_score", "kills", "score", "hits_taken", "enemy_fire", "in_range_msg",
      "gun_burst", "bullet_timer", "status_msg", "explosion_colour", "enemy_table"]),
    ("editor", """Editor / user modes (3.16): editor_values is the 41h-byte preset image
 *   (2050 N, 2052 E, 2054 alt, 2056/58/5A pitch/bank/heading, 205C speed, 205E throttle,
 *   2060-2066 controls); usermode_slots holds the saved presets.""",
     ["editor_active", "editor_key", "editor_key_table", "editor_row", "editor_page",
      "editor_values", "usermode_cur", "usermode_slots", "demo_mode", "reality_mode", "season",
      "clock_hour", "clock_min", "editor_value", "editor_digits", "pow10_lo"]),
    ("view and scenery", """3D view, scenery interpreter and back buffer (3.3-3.8).""", []),
    ("system", """Timer, keyboard, sound, disk and frame bookkeeping.""", []),
]


def block_of(off):
    if 0x0832 <= off < 0x0900:
        return "flight model"
    if 0x0900 <= off < 0x1D40:
        return "panel"
    if 0x1D40 <= off < 0x1DC0 or 0x3000 <= off:
        return "view and scenery"
    if 0x1DC0 <= off < 0x1E24:
        return "engine"
    if 0x1E24 <= off < 0x2000:
        return "war" if off >= 0x1E50 else "aircraft"
    if 0x2000 <= off < 0x3000:
        return "editor"
    return "system"


def load():
    syms = {}
    for line in open(SYMBOLS, encoding="utf-8"):
        m = VAR.match(line.strip())
        if not m:
            continue
        off, name, typ, comment = int(m.group(1), 16), m.group(2), m.group(3), m.group(4) or ""
        prev = syms.get(name)
        if prev and prev[0] != off:
            sys.exit(f"gen_state: {name} defined at {prev[0]:04X} and {off:04X}")
        if prev and prev[1] and not typ:
            continue
        syms[name] = (off, typ, comment.strip())
    return syms


def signed(name, comment):
    return name in SIGNED or "[signed]" in comment


def accessors(name, off, typ, comment):
    U = name.upper()
    o = []
    if typ == "byte":
        o.append(f"static inline uint8_t gs_{name}(Pc *pc) {{ return gs_rd8(pc, GS_{U}); }}")
        o.append(f"static inline void gs_set_{name}(Pc *pc, uint8_t v) {{ gs_wr8(pc, GS_{U}, v); }}")
    elif typ == "word":
        t = "int16_t" if signed(name, comment) else "uint16_t"
        o.append(f"static inline {t} gs_{name}(Pc *pc) {{ return ({t})gs_rd16(pc, GS_{U}); }}")
        o.append(f"static inline void gs_set_{name}(Pc *pc, {t} v) {{ gs_wr16(pc, GS_{U}, (uint16_t)v); }}")
    elif typ == "dword":
        t = "int32_t" if signed(name, comment) else "uint32_t"
        o.append(f"static inline {t} gs_{name}(Pc *pc) {{ return ({t})gs_rd32(pc, GS_{U}); }}")
        o.append(f"static inline void gs_set_{name}(Pc *pc, {t} v) {{ gs_wr32(pc, GS_{U}, (uint32_t)v); }}")
    elif typ == "24-bit":
        o.append(f"static inline uint32_t gs_{name}(Pc *pc) {{ return gs_rd24(pc, GS_{U}); }}")
        o.append(f"static inline void gs_set_{name}(Pc *pc, uint32_t v) {{ gs_wr24(pc, GS_{U}, v); }}")
    elif typ == "table":
        o.append(f"static inline uint8_t gs_{name}_b(Pc *pc, unsigned i) {{ return gs_rd8(pc, (uint16_t)(GS_{U} + i)); }}")
        o.append(f"static inline uint16_t gs_{name}_w(Pc *pc, unsigned i) {{ return gs_rd16(pc, (uint16_t)(GS_{U} + 2 * i)); }}")
        o.append(f"static inline void gs_set_{name}_b(Pc *pc, unsigned i, uint8_t v) {{ gs_wr8(pc, (uint16_t)(GS_{U} + i), v); }}")
        o.append(f"static inline void gs_set_{name}_w(Pc *pc, unsigned i, uint16_t v) {{ gs_wr16(pc, (uint16_t)(GS_{U} + 2 * i), v); }}")
    elif typ == "text":
        o.append(f"static inline uint8_t gs_{name}_ch(Pc *pc, unsigned i) {{ return gs_rd8(pc, (uint16_t)(GS_{U} + i)); }}")
        o.append(f"static inline void gs_set_{name}_ch(Pc *pc, unsigned i, uint8_t v) {{ gs_wr8(pc, (uint16_t)(GS_{U} + i), v); }}")
    return o


HEAD = """/* GENERATED by tools/gen_state.py from docs/symbols.txt -- do not edit by hand.
 * To change a name, type or comment, edit docs/symbols.txt (`var 0618:XXXX name TYPE ; comment`,
 * add `[signed]` to the comment for a signed word/dword) and run `python tools/gen_state.py`.
 *
 * Game state accessors (subphase 3.15). The game data segment DS = 0618 is the only copy of
 * the state: the original code still runs for every routine without a native, so nothing is
 * cached in C. Each accessor reads or writes DS:offset (see GS_SEG) through cpu_read8/16 and cpu_write8/16, which
 * keeps tracing and the --verify write log working.
 *
 *   GS_NAME                      offset of the variable in DS (for tables, pointers, SI/DI)
 *   gs_NAME(pc), gs_set_NAME(pc, v)          byte / word / dword / 24-bit variables
 *   gs_NAME_b/_w(pc, i), gs_set_NAME_b/_w    tables: byte i / word i from GS_NAME
 *   gs_NAME_ch(pc, i), gs_set_NAME_ch        text: character i
 *
 * Formats used throughout (see docs/symbols.txt and docs/subphases/3.*.md):
 *   angles      16-bit binary, 10000h = 360 deg (signed view: 8000h = -180 deg)
 *   sin / cos   Q15, 7FFFh = +1.0
 *   controls    signed 16-bit, 7FFFh = full
 *   airspeed    V; knots = V * 259h / 10000h (about V / 109)
 *   altitude    dword 30D5: byte fraction + word integer altitude, ground = 3
 *   position    dwords 16.16; the integer word is in 256 m units
 *
 * ==== state blocks (data model) ==========================================================
"""


def main():
    syms = load()
    by_group = {g[0]: [] for g in GROUPS}
    listed = {n: g[0] for g in GROUPS for n in g[2]}
    for name, (off, typ, comment) in syms.items():
        by_group[listed.get(name) or block_of(off)].append(name)

    out = [HEAD]
    for title, desc, _ in GROUPS:
        names = sorted(by_group[title], key=lambda n: syms[n][0])
        out.append(f" *\n * -- {title} --\n * {desc}\n")
        for n in names:
            off, typ, comment = syms[n]
            t = typ or "-"
            if typ in ("word", "dword") and signed(n, comment):
                t = "s" + typ
            c = f"  {comment}" if comment else ""
            out.append(f" *   {off:04X} {n:22s} {t:6s}{c}".rstrip() + "\n")
    out.append(""" */
#ifndef FS1_GAME_STATE_H
#define FS1_GAME_STATE_H

#include <stdint.h>

#include "native.h"

/* The segment the original addresses its variables through: DS, which is GAME_DS (0618) in
 * normal operation but not in every caller (start-up, editor presets), so the accessors use
 * the live DS register exactly as the replaced instructions did. */
#define GS_SEG(pc) ((pc)->cpu.sregs[S_DS])

static inline uint8_t gs_rd8(Pc *pc, uint16_t off) { return cpu_read8(&pc->cpu, cpu_linear(GS_SEG(pc), off)); }
static inline uint16_t gs_rd16(Pc *pc, uint16_t off) { return cpu_read16(&pc->cpu, cpu_linear(GS_SEG(pc), off)); }
static inline uint32_t gs_rd32(Pc *pc, uint16_t off)
{
    return gs_rd16(pc, off) | (uint32_t)gs_rd16(pc, (uint16_t)(off + 2)) << 16;
}
static inline uint32_t gs_rd24(Pc *pc, uint16_t off)
{
    return gs_rd16(pc, off) | (uint32_t)gs_rd8(pc, (uint16_t)(off + 2)) << 16;
}
static inline void gs_wr8(Pc *pc, uint16_t off, uint8_t v) { cpu_write8(&pc->cpu, cpu_linear(GS_SEG(pc), off), v); }
static inline void gs_wr16(Pc *pc, uint16_t off, uint16_t v) { cpu_write16(&pc->cpu, cpu_linear(GS_SEG(pc), off), v); }
static inline void gs_wr32(Pc *pc, uint16_t off, uint32_t v)
{
    gs_wr16(pc, off, (uint16_t)v);
    gs_wr16(pc, (uint16_t)(off + 2), (uint16_t)(v >> 16));
}
static inline void gs_wr24(Pc *pc, uint16_t off, uint32_t v)
{
    gs_wr16(pc, off, (uint16_t)v);
    gs_wr8(pc, (uint16_t)(off + 2), (uint8_t)(v >> 16));
}
""")
    for title, _, _ in GROUPS:
        names = sorted(by_group[title], key=lambda n: syms[n][0])
        out.append(f"\n/* ---- {title} {'-' * (84 - len(title))} */\n\n")
        for n in names:
            off, typ, comment = syms[n]
            out.append(f"#define GS_{n.upper()} 0x{off:04X}\n")
        out.append("\n")
        for n in names:
            off, typ, comment = syms[n]
            for a in accessors(n, off, typ, comment):
                out.append(a + "\n")
    out.append("\n#endif\n")
    text = "".join(out)

    if "--check" in sys.argv:
        # text mode: CRLF from a Windows checkout reads as LF
        cur = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if cur != text:
            print("src/game/state.h is out of date: run python tools/gen_state.py")
            sys.exit(1)
        return
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {len(syms)} symbols")


if __name__ == "__main__":
    main()
