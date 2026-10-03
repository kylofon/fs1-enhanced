"""Run the trace-campaign sessions with --verify and report native-vs-original mismatches.

usage: python tools/verify_campaign.py [NAMES] [-- EXTRA_ARGS...]

NAMES is a comma-separated list of natives to verify (default: all). Each name is also
enabled with --native-on, so default-off natives such as test_passthrough get checked.
EXTRA_ARGS are passed to fs1.exe as is. Sessions run in parallel. Prints a table per
native (calls and mismatches per session, then totals) and exits 1 on any mismatch.
"""
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_campaign import EXE, HEADLESS_ENV, ROOT, sessions  # noqa: E402

SUMMARY = re.compile(r"verify-summary (\S+) calls (\d+) mismatches (\d+)")

# Natives that stay enabled but are not verified in a session, because --verify cannot check
# them there: it single-steps the original until it returns, and these calls never return.
#   reboot / int9_keyboard, key_dispatch: Ctrl+Alt+Del. The original key_dispatch jumps to
#   the BIOS reset entry inside the keyboard interrupt and the machine reboots while --verify
#   steps it (see docs/subphases/3.18.md). key_dispatch is reached by the CPU (and so
#   verified) when int9_keyboard hands a call back to the original. That session is checked
#   by comparing screenshots with the natives on and off instead.
NO_VERIFY = {"reboot": {"int9_keyboard", "key_dispatch"}}


def all_natives():
    r = subprocess.run([EXE, "--list-natives"], cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True)
    out = (r.stdout + r.stderr).splitlines()
    return [m.group(1) for m in (re.match(r"(\S+)\s+[0-9A-F]{4}:[0-9A-F]{4}\s", l) for l in out) if m]


def run(session, names, extra):
    name, args, frames = session
    cmd = [EXE, "--frames", str(frames)]
    skip = NO_VERIFY.get(name, set())
    if skip and names == ["all"]:
        cmd += ["--native-on", "all"]
        for n in all_natives():
            if n not in skip:
                cmd += ["--verify", n]
    else:
        for n in names:
            cmd += ["--native-on", n] + (["--verify", n] if n not in skip else [])
    cmd += extra + args
    r = subprocess.run(cmd, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True, timeout=3600)
    out = r.stdout + r.stderr
    results = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in SUMMARY.finditer(out)}
    reports = [l for l in out.splitlines() if l.startswith("verify ")]
    return name, r.returncode, results, reports


def main():
    argv = sys.argv[1:]
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    names = argv[0].split(",") if argv else ["all"]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        done = list(pool.map(lambda s: run(s, names, extra), sessions()))

    natives = sorted({n for _, _, res, _ in done for n in res})
    failed = False
    for _, code, res, _ in done:
        failed |= code != 0 or not res
    for native in natives:
        print(f"\n{native}")
        print(f"  {'session':16s} {'calls':>10s} {'mismatches':>10s}")
        tc = tm = 0
        for sname, code, res, _ in done:
            c, m = res.get(native, (0, 0))
            tc, tm = tc + c, tm + m
            print(f"  {sname:16s} {c:10d} {m:10d}" + (f"  (exit {code})" if code else ""))
        print(f"  {'TOTAL':16s} {tc:10d} {tm:10d}")
        failed |= tm > 0
    for sname, code, res, reports in done:
        if not res:
            print(f"{sname}: no verify summary (exit {code})")
        for line in reports[:5]:
            print(f"{sname}: {line}")
    for sname, skip in NO_VERIFY.items():
        print(f"not verified in {sname} (enabled, see NO_VERIFY): {', '.join(sorted(skip))}")
    print("\nFAIL" if failed else "\nOK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
