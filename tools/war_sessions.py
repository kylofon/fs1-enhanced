"""Long war-mode sessions for verifying the 3.17 natives (docs/subphases/3.17.md).

usage: python tools/war_sessions.py [NAMES] [-- EXTRA_ARGS...]
       python tools/war_sessions.py --coverage TRACE_FILE

The trace campaign's war session (tools/trace_campaign.py) never meets an enemy: it ends
before the aircraft leaves the home field. These sessions take off, turn west towards the
enemy fields and fly there, firing, bombing and getting hit on the way:

  war_attack   take off, declare war (W), fly west, fire bursts (Space) the whole way
  war_bomb     the same, and drop the five bombs (X) over the enemy fields
  war_long     a longer flight that circles over the enemy fields while firing

They run like tools/verify_campaign.py: NAMES (comma separated, default: the war natives)
are enabled and verified; EXTRA_ARGS go to fs1 as they are (e.g. "--native-off war_frame"
to verify the routines that war_frame calls from C). With --coverage, the sessions run
with every native off and without --verify, and write an execution trace map to TRACE_FILE instead, to see which war
code paths were reached (python tools/war_sessions.py --coverage x.bin, then compare it to
the S marks in extracted/fs1.asm).
"""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_campaign import EXE, HEADLESS_ENV, ROOT, editor_set  # noqa: E402
from verify_campaign import SUMMARY  # noqa: E402

WAR_NATIVES = ["war_frame", "war_enemy_update", "war_guns", "war_bomb_fall", "war_ammo_dec", "war_score_add",
               "war_status", "war_scope_clear", "ground_service", "war_ground_reset"]


def takeoff_west():
    """Editor: Europe 1917 on. Full throttle, W, pull up after ~25 s, bank left into a turn
    to the west and level out. Returns (keys, frame after the turn)."""
    k = ["120:A", "180:B"]
    ks, f = editor_set(300, 6, 1)
    k += ks
    t = f
    k += [f"{t}:F2*2", f"{t + 60}:W*2"]
    k += [f"{t + d}:Keypad 2*2" for d in (1500, 1530, 1560, 2200, 2230)]
    k += [f"{t + 3300}:Keypad 4*2", f"{t + 3700}:Keypad 6*2", f"{t + 4100}:Keypad 6*2", f"{t + 4400}:Keypad 4*2"]
    return k, t + 4500


def session_attack():
    k, f = takeoff_west()
    k += [f"{f + d}:Space*4" for d in range(0, 12000, 150)]
    return "war_attack", ["--keys", ",".join(k)], f + 12000


def session_bomb():
    k, f = takeoff_west()
    k += [f"{f + d}:Space*4" for d in range(0, 12000, 300)]
    k += [f"{f + d}:X*2" for d in range(3000, 12000, 1500)]
    return "war_bomb", ["--keys", ",".join(k)], f + 12000


def session_long():
    k, f = takeoff_west()
    k += [f"{f + d}:Space*4" for d in range(0, 16000, 200)]
    # a slow left circle over the enemy fields
    k += [f"{f + 5000}:Keypad 4*2", f"{f + 5200}:Keypad 6*2", f"{f + 9000}:Keypad 4*2", f"{f + 9200}:Keypad 6*2"]
    return "war_long", ["--keys", ",".join(k)], f + 16000


def sessions():
    return [session_attack(), session_bomb(), session_long()]


def run(session, names, extra, trace=None):
    name, args, frames = session
    cmd = [EXE, "--frames", str(frames)]
    if trace:
        cmd += ["--trace", trace]
    else:
        for n in names:
            cmd += ["--native-on", n, "--verify", n]
    cmd += extra + args
    r = subprocess.run(cmd, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True, timeout=7200)
    out = r.stdout + r.stderr
    results = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in SUMMARY.finditer(out)}
    reports = [line for line in out.splitlines() if line.startswith("verify ")]
    return name, r.returncode, results, reports


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--coverage":
        trace = os.path.abspath(argv[1])
        for s in sessions():  # sequential: the trace file is merged after each run
            name, code, _, _ = run(s, [], ["--native-off", "all"], trace)
            print(f"{name}: exit {code}")
        return
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    names = argv[0].split(",") if argv else WAR_NATIVES
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        done = list(pool.map(lambda s: run(s, names, extra), sessions()))
    failed = False
    for native in sorted({n for _, _, res, _ in done for n in res}):
        print(f"\n{native}")
        tc = tm = 0
        for sname, code, res, _ in done:
            c, m = res.get(native, (0, 0))
            tc, tm = tc + c, tm + m
            print(f"  {sname:16s} {c:10d} {m:10d}" + (f"  (exit {code})" if code else ""))
        print(f"  {'TOTAL':16s} {tc:10d} {tm:10d}")
        failed |= tm > 0
    for sname, code, res, reports in done:
        failed |= code != 0 or not res
        for line in reports[:10]:
            print(f"{sname}: {line}")
    print("\nFAIL" if failed else "\nOK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
