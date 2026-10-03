"""Attitude sessions for subphase 3.5 (horizon); see docs/subphases/3.5.md.

usage: python tools/horizon_sessions.py [verify NAMES] [shots TAG] [EXTRA fs1 ARGS...]  (env ONLY=prefix)

Each session: optional user mode (3 = dusk), slew on, altitude 3000, then slew attitude keys. In
slew mode KP7/KP9 add/subtract 80h to the bank rate, KP1/KP3 to the heading rate, and the flap
keys F1..F9 set the pitch rate ((flaps - 40h) * 4 per update, F5 = 0). Screenshots go to
extracted/horizon/."""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "extracted", "horizon")
sys.path.insert(0, os.path.join(ROOT, "tools"))
from trace_campaign import EXE, HEADLESS_ENV, editor_set  # noqa

# editor rows (Return presses from the top); North and East take two Returns each
USER_MODE, SLEW, ALT, PITCH, BANK, HEADING, AIRSPEED = 0, 3, 12, 13, 14, 15, 16


def session(name, events, mode=None, fields=(), length=1500, shots=(0,), slew=True, alt=3000):
    k = ["120:A", "180:B"]
    f = 300
    if mode is not None:
        ks, f = editor_set(f, USER_MODE, mode)
        k += ks
    if slew:
        ks, f = editor_set(f, SLEW, 1)
        k += ks
    for fld, val in [(ALT, alt)] + list(fields):
        ks, f = editor_set(f, fld, val)
        k += ks
    for off, key, hold in events:
        k.append(f"{f + off}:{key}*{hold}")
    return name, k, [f + s for s in shots], f + length


def sessions():
    sh = (100, 300, 500, 700, 900, 1100, 1300)
    roll = [(100, "Keypad 7", 2), (140, "Keypad 7", 2)]
    roll_rev = [(100, "Keypad 9", 2), (140, "Keypad 9", 2), (180, "Keypad 9", 2)]
    pitch_up = [(100, "F1", 2)]
    pitch_dn = [(100, "F9", 2)]
    loop = ([(20, "F2", 2)] + [(60 + 10 * i, "Keypad 8", 2) for i in range(4)]
            + [(900 + 10 * i, "Keypad 2", 2) for i in range(16)])
    lsh = tuple(range(600, 5000, 300))
    turn = [(60, "Keypad 1", 2), (90, "Keypad 1", 2)]
    out = [
        session("level", [], shots=sh),
        session("roll", roll, shots=sh),
        session("roll_rev", roll_rev, shots=sh),
        session("pitch_up", pitch_up, shots=sh),
        session("pitch_down", pitch_dn, shots=sh),
        session("pitch_up_slow", [(100, "F3", 2)], length=2500, shots=sh),
        session("roll_pitch", roll + pitch_up + turn, shots=sh),
        session("roll_rev_pitch_down", roll_rev + pitch_dn + turn, shots=sh),
        session("tumble", roll + [(100, "F7", 2)] + turn, length=3000, shots=sh),
        session("dusk_roll", roll, mode=3, shots=sh),
        session("dusk_pitch", pitch_up, mode=3, shots=sh),
        session("dusk_mix", roll_rev + [(100, "F3", 2)] + turn, mode=3, length=3000, shots=sh),
        session("flight_dive", loop, slew=False, alt=8000, length=5000, shots=lsh),
        session("dusk_flight_dive", loop, mode=3, slew=False, alt=8000, length=5000,
                shots=lsh),
        session("editor_pitch20", [], fields=[(PITCH, 20)], shots=sh),
        session("editor_bank45", [], fields=[(BANK, 45)], shots=sh),
    ]
    return out


def run(s, extra, tag):
    name, k, shots, frames = s
    cmd = [EXE, "--frames", str(frames), "--keys", ",".join(k)] + extra
    if tag:
        cmd += ["--screenshot", os.path.join(OUT, f"{tag}_{name}")]
        for f in shots:
            cmd += ["--shot-at", str(f)]
    r = subprocess.run(cmd, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True)
    return name, [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("verify") or "unknown" in l]


if __name__ == "__main__":
    args = sys.argv[1:]
    extra, tag = [], None
    if args and args[0] == "verify":
        for n in args[1].split(","):
            extra += ["--verify", n]
        args = args[2:]
    if args and args[0] == "shots":
        tag = args[1]
        args = args[2:]
    extra += args
    os.makedirs(OUT, exist_ok=True)
    only = os.environ.get("ONLY")
    ss = [s for s in sessions() if not only or s[0].startswith(only)]
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as p:
        for name, lines in p.map(lambda s: run(s, extra, tag), ss):
            cyc = [l for l in lines if l.startswith("verify-cyc")]
            rest = [l for l in lines if not l.startswith("verify-cyc")]
            print(f"{name:22s} cycdiffs {len(cyc)}", *rest[:8], *cyc[:3], sep="\n   ")
