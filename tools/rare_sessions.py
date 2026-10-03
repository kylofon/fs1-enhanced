"""Rare-path sessions (PLAN.md S3F.4): reach code that no flown or scripted session reaches, by
writing game state with --poke (src/main.c) at chosen frames, and verify the natives there.

usage: python tools/rare_sessions.py [NAMES] [-- EXTRA_ARGS...]
       python tools/rare_sessions.py --coverage [SESSIONS]

Verify mode runs every session with NAMES (default: all) enabled and verified, like
tools/verify_campaign.py, and exits 1 on any mismatch. --coverage runs the sessions (default:
all, or a comma-separated list) with every native off and an execution trace, and prints for
each target range of the listing how many of its instructions ran (work/rare_<session>.bin).

The pokes stand in for situations that are hard to fly: engine faults and leaking tanks, an
empty tank and an engine restart, carb ice, random failures (reliability 0), the airspeed caps
(>= 6400h, overflow, negative = the "512 kt" case), wing damage, the loop-over mirror, the
scenery opcodes no shipped area uses (a bytecode stub patched into area 0), and in war mode
an enemy shot down, a bomb hit, enemy fire and every damage level, and an enemy in the view
(record 0 copied from the aircraft / eye position each frame with --poke OFF=@SRC).
Offsets are DS 0618 (docs/symbols.txt).
"""
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_campaign import EXE, HEADLESS_ENV, ROOT  # noqa: E402
from verify_campaign import SUMMARY  # noqa: E402
from war_sessions import takeoff_west  # noqa: E402

ASM = os.path.join(ROOT, "extracted", "fs1.asm")
CODE_BASE = 0x500  # segment 0050
T_EXEC = 1

# Menu A/B, full throttle, release brakes, rotate: airborne well before frame 4600.
TAKEOFF = ["120:A", "180:B", "300:F2*2", "310:.*2", "1500:Keypad 2*60"]
AIRBORNE = 4600


def session(name, keys, pokes, frames, targets):
    args = ["--keys", ",".join(keys)]
    if pokes:
        args += ["--poke", ",".join(pokes)]
    return {"name": name, "args": args, "frames": frames, "targets": targets}


def sessions():
    f = AIRBORNE
    return [
        # engine_faults 0F: oil leak + no pressure (power cap 0FFF, hot oil), both tanks leak
        # until they clamp at 0 and the engine stops; then refuelled in reality mode.
        session("engine_faults", TAKEOFF,
                [f"{f}:1DC8=0F", f"{f + 400}:1DC8=00", f"{f + 600}:1DCE=23", f"{f + 600}:1DD1=23",
                 f"{f + 600}:204C=01*2000"], f + 1500,
                {"fault tests 2DC3..2E37": (0x2DC3, 0x2E3D), "tank leaks 2EAD..2ED5": (0x2EAD, 0x2EDA)}),
        # On the ground in reality mode: empty both tanks (engine stops), refuel, hold the
        # magnetos on START: the starter path, once in the default season and once in season 1.
        session("engine_start", ["120:A", "180:B"],
                ["300:204C=01*3000", "600:1DCE=00", "600:1DD1=00", "700:1DCE=23", "700:1DD1=23",
                 "800:0583=04*200", "1300:1DCE=00", "1300:1DD1=00", "1400:1DCE=23", "1400:1DD1=23",
                 "1400:206A=01*600", "1500:0583=04*200"], 2200,
                {"starter 2D47..2D6E": (0x2D47, 0x2D70), "restart 2D70": (0x2D70, 0x2D7B)}),
        # Carb ice: carb heat byte set with ice present (the 2000h throttle loss), then the
        # natural ice trigger in sub_04FB (reality, dusk, season 2, low throttle, counter phase).
        session("carb_ice", TAKEOFF,
                [f"{f}:0577=01", f"{f}:1DCA=14", f"{f + 300}:0577=00",
                 f"{f + 400}:204C=01*400", f"{f + 400}:0412=0002*30", f"{f + 400}:206A=02*30",
                 f"{f + 400}:041A=01*30", f"{f + 400}:03F6=0000*30", f"{f + 400}:0597=2000*30"], f + 900,
                {"ice loss 2DAF..2DB2": (0x2DAF, 0x2DB6), "ice trigger 059D..05C6": (0x059D, 0x05C7)}),
        # Random failures: reality mode, reliability 0, the 60-frame countdown forced every frame.
        session("failures", TAKEOFF,
                [f"{f}:204C=01*1500", f"{f}:2089=00*1500", f"{f}:0408=0000*1500"], f + 1600,
                {"sub_05D4 05D4..05ED": (0x05D4, 0x05EF), "failure handlers 05EF..0624": (0x05EF, 0x0625)}),
        # Airspeed caps: above 6400h, signed overflow, negative (DA20h = 512 kt as unsigned).
        session("overspeed", TAKEOFF,
                [f"{f}:08A8=6500", f"{f + 60}:08A8=7FF0", f"{f + 120}:08A8=DA20"], f + 400,
                {"caps 18FB..1904": (0x18FB, 0x1906)}),
        # Wing damage flag, then pitch past +90 and past -90 degrees (fix_loop_over mirror).
        session("loop_over", TAKEOFF,
                [f"{f}:1E83=01", f"{f + 60}:1E83=00", f"{f + 120}:0838=4800", f"{f + 240}:0838=B800"], f + 400,
                {"wing damage 1684..": (0x1684, 0x1690), "mirror 18D8..18F6": (0x18D8, 0x18FA)}),
        # Scenery opcodes that no shipped area uses (05 06 08 0D 0E 10 11 15 2A 34): a stub at
        # DS:5B80 (free space after area 0) runs them, repeats the area's first instruction
        # (25 15 09 00 00) and jumps back to 3A89; the entry jump 0B FC 20 replaces that
        # instruction at 3A84. Stays in effect until the next area load.
        session("scenery_ops", ["120:A", "180:B"], [f"400:{a:04X}={v}" for a, v in SCENERY_STUB] +
                ["401:3A84=FC0B", "401:3A86=20"], 700,
                {"05 3E1A": (0x3E1A, 0x3E47), "06 3E47": (0x3E47, 0x3E69), "08 3E69": (0x3E69, 0x3E70),
                 "0D 3E78": (0x3E78, 0x3E98), "0E 3E98": (0x3E98, 0x3EAF), "10 3EAF": (0x3EAF, 0x3EB6),
                 "11 3EB6": (0x3EB6, 0x3EBA), "34 41CB": (0x41CB, 0x41D0), "2A 43A1": (0x43A1, 0x43C0)}),
        *war_sessions(),
    ]


# The stub, one opcode per entry (lengths from docs/SCENERY_FORMAT.md; every handler advances
# the IP itself, and 0B adds r to the IP of the 0B).
_OPS = ["11", "34 00", "08", "0E 4F CC 50 35", "10", "06 10 10 40 30",
        "0D 80 5C", "00 00 00 00 00 64 00", "0D 00 00",  # dot (0,0,100) while capturing
        "2A 00 00 00 00 64 00 64 00 00 00 64 00 08",  # 8 dots (100,0,100) -> (0,0,100)
        "08", "15", "05 00 00 64 00 00 00 00 00 00 00 00 00",  # eye (0,100,0), angles 0
        "25 15 09 00 00"]  # the area's first instruction, replaced by the entry jump
_STUB = bytes.fromhex(" ".join(_OPS))
_STUB += bytes([0x0B]) + ((0x3A89 - 0x5B80 - len(_STUB)) & 0xFFFF).to_bytes(2, "little")  # back to 3A89
_STUB += bytes(len(_STUB) % 2)  # pad to whole words
SCENERY_STUB = [(0x5B80 + i, f"{_STUB[i] | _STUB[i + 1] << 8:04X}") for i in range(0, len(_STUB), 2)]


def war_sessions():
    """War paths that need an enemy where the scripted flight never has one (see
    docs/subphases/3.17.md and src/natives/war.c). Enemy record 0 is at 1F3B: +A (1F45) is its
    in-range flag; bullets with life < 0Ah hit any record whose flag is set."""
    k, f = takeoff_west()
    out = [
        # Shoot one down (fire = 1E7C 3, record 0 in range), four times, then the score wrap.
        session("war_kill", k,
                [f"{f}:1E7C=03", f"{f}:1F45=01*12", f"{f + 200}:1E7C=03", f"{f + 200}:1F45=01*40",
                 f"{f + 400}:1E57=2710*12", f"{f + 400}:1E7C=03", f"{f + 400}:1F45=01*12"], f + 700,
                {"shot down 34CD..34E3": (0x34CD, 0x34E4), "explosion 35AB..": (0x35AB, 0x35B0)}),
        # Bomb hit: a falling bomb low enough while the scenery says a target is below.
        session("war_bomb_hit", k, [f"{f}:0575=0001", f"{f}:1E53=0064"], f + 300,
                {"bomb hit 3534..3544": (0x3534, 0x3545)}),
    ]
    # Enemy fire and an enemy in the view need record 0 next to or ahead of the aircraft: each
    # frame its position is copied from the aircraft's (24-bit pos+1 east/alt/north at
    # 30D2/30D6/30DA, the units of the record's 1F3B/1F3E/1F41) with an offset. Attack state, no
    # movement, no give-up (30D5: pos_east+2 - [F] < 0 and pos_north+2 - [11] >= 0, signed).
    g = f + 200

    def enemy(start, n, d_east, fire_dist, eye=False, d_north=0):
        w = lambda off, v: f"{start}:{off}={v}*{n}"
        out = [w("1F54", "02"), w("1F4E", "00"), w("1F4F", "00"), w("1F50", "00"), w("1F55", "00"),
               w("1F4A", "@30D3+100"), w("1F4C", "@30DB-100"), w("1F51", fire_dist),
               w("1F3B", f"@30D2.3{d_east:+X}"), w("1F3E", "@30D6.3"), w("1F41", "@30DA.3")]
        if eye:  # the view code (317A) compares only the low words, with eye_pos 30EB/30ED/30EF
            out += [w("1F3B", f"@30EB{d_east:+X}"), w("1F3E", "@30ED"), w("1F41", f"@30EF{d_north:+X}")]
        return out

    # Fire every frame from the aircraft's own position: hits_taken climbs through every
    # damage threshold, then again with kills held at 4 for the other branch.
    fire_pokes = enemy(g, 300, 0, "FF") + [f"{g}:1E5C=00", f"{g + 300}:1E5C=00", f"{g + 300}:1E50=04*300"]
    fire_pokes += enemy(g + 300, 300, 0, "FF")
    out.append(session("war_fire", k, fire_pokes, g + 600,
                       {"enemy fire 30D5..310F": (0x30D5, 0x3110), "damage 35DB..3630": (0x35DB, 0x3635)}))
    # In the view: the enemy held ahead (eye_pos - east) at shrinking distances: a dot far away, the
    # sprite size classes, then in range (state 2, forward view). Never fires.
    view_pokes, t = [], g
    for d in (0x1000, 0x400, 0x200, 0x180, 0x100, 0xA0, 0x70, 0x50, 0x30, 0x18):
        view_pokes += enemy(t, 60, -d, "00", eye=True)
        t += 60
    # The flight heads about 304 deg; hold it due west (C000h), wings level, so ahead is -east.
    view_pokes += [f"{g}:0870=C000*{t - g}", f"{g}:083C=0000*{t - g}"]
    out.append(session("war_view", k + [f"{g + d}:Space*4" for d in range(0, t - g, 50)], view_pokes, t,
                       {"in view 31E5..3269": (0x31E5, 0x326A), "sprites 5BAA..5C6D": (0x5BAA, 0x5C6E)}))
    return out


def run(s, names, extra, trace=None):
    cmd = [EXE, "--frames", str(s["frames"])]
    if trace:
        cmd += ["--trace", trace]
    else:
        for n in names:
            cmd += ["--native-on", n, "--verify", n]
    cmd += extra + s["args"]
    r = subprocess.run(cmd, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True, timeout=7200)
    out = r.stdout + r.stderr
    results = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in SUMMARY.finditer(out)}
    reports = [line for line in out.splitlines() if line.startswith("verify ")]
    return s["name"], r.returncode, results, reports


def listing_insns():
    """Instruction offsets of the listing (data lines included as one entry per line)."""
    offs = []
    with open(ASM, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r"\s{4}([0-9A-F]{4})  ", line)
            if m:
                offs.append(int(m.group(1), 16))
    return offs


def coverage(names):
    os.makedirs(os.path.join(ROOT, "work"), exist_ok=True)
    chosen = [s for s in sessions() if not names or s["name"] in names]
    jobs = []
    for s in chosen:
        path = os.path.join(ROOT, "work", f"rare_{s['name']}.bin")
        if os.path.exists(path):
            os.remove(path)
        jobs.append((s, path))
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        done = list(pool.map(lambda j: run(j[0], [], ["--native-off", "all"], j[1]), jobs))
    offs = listing_insns()
    for (s, path), (_, code, _, _) in zip(jobs, done):
        trace = open(path, "rb").read() if os.path.exists(path) else b""
        print(f"{s['name']}: exit {code}")
        for label, (lo, hi) in s["targets"].items():
            ins = [o for o in offs if lo <= o < hi]
            ran = sum(1 for o in ins if trace and trace[CODE_BASE + o] & T_EXEC)
            # bytes too: ranges listed as data (never decoded) are one line per 16 bytes
            nb = sum(1 for o in range(lo, hi) if trace and trace[CODE_BASE + o] & T_EXEC)
            print(f"  {label:32s} {ran}/{len(ins)} listing lines, {nb} executed bytes")


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--coverage":
        coverage(argv[1].split(",") if len(argv) > 1 else [])
        return
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    names = argv[0].split(",") if argv else ["all"]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        done = list(pool.map(lambda s: run(s, names, extra), sessions()))
    failed = False
    for native in sorted({n for _, _, res, _ in done for n in res}):
        rows = [(sname, code, *res.get(native, (0, 0))) for sname, code, res, _ in done]
        tc, tm = sum(r[2] for r in rows), sum(r[3] for r in rows)
        failed |= tm > 0
        if tm:
            print(f"\n{native}")
            for sname, code, c, m in rows:
                print(f"  {sname:16s} {c:10d} {m:10d}" + (f"  (exit {code})" if code else ""))
    for sname, code, res, reports in done:
        failed |= code != 0 or not res
        print(f"{sname}: exit {code}, {sum(c for c, _ in res.values())} verified calls, "
              f"{sum(m for _, m in res.values())} mismatches")
        for line in reports[:10]:
            print(f"{sname}: {line}")
    print("\nFAIL" if failed else "\nOK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
