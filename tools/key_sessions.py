"""Key sessions for the 3.18 natives (keyboard interrupt, key dispatch, key handlers; see
docs/subphases/3.18.md).

usage: python tools/key_sessions.py [NAMES] [-- EXTRA_ARGS...]     verify
       python tools/key_sessions.py --shots                       screenshots, natives on vs off

The trace campaign's flight_keys session presses every flight key once. These sessions also
press them in slew mode, war mode and the radar view, select the radios twice in a row (second
field), wrap the frequencies, transponder digits and OBI course, pick view directions (Scroll
Lock, then a keypad key), hold keys (typematic repeat), type in the editor (Enter, digits,
Backspace, '-', '*', Insert, Esc) and reboot with Ctrl+Alt+Del.

Verify mode works like tools/verify_campaign.py: NAMES (comma separated, default: every native
of src/natives/keys.c, i.e. at 0050:0955-10B5) are enabled and verified; the reboot session
does not verify int9_keyboard and key_dispatch (Ctrl+Alt+Del never returns; see NO_VERIFY
there).

--shots runs these sessions and the trace campaign's twice without --verify, with all natives
on and with the keys natives off, saves screenshots every 300 frames and at the end, and
reports any that differ.
"""
import filecmp
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_campaign import EXE, HEADLESS_ENV, ROOT, editor_set, keys_every  # noqa: E402
from trace_campaign import sessions as campaign_sessions  # noqa: E402
from verify_campaign import NO_VERIFY, SUMMARY  # noqa: E402

START = ["120:A", "180:B"]
KEYPAD = ["Keypad 8", "Keypad 9", "Keypad 6", "Keypad 3", "Keypad 2", "Keypad 1", "Keypad 4", "Keypad 7",
          "Keypad 5"]


def seq(start, step, names, hold=2):
    return keys_every(start, step, names, hold)


def session_flight():
    k, f = list(START), 300
    groups = [
        # every bound and some unbound keys, one by one
        ["F2", "F4", "F6", "F8", "F8", "F8", "F10", "F8", "F6", "F4", "F1", "F3", "F5", "F7", "F9", "F1"],
        KEYPAD + ["Keypad 0", "Keypad +", "Keypad 5", "Keypad 7", "Keypad 1"],
        ["Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown", "Insert", "Keypad 5"],
        ["1", "2", "3", "4", "5", "0", "4", "G", "H", "H", "L", "L", "D", "A", "M", ".", "Return",
         "Q", "Z", "B", "6", "Backspace", "Keypad -", "Keypad .", "Tab", "Tab", "W", "X", "Space", "Left Alt"],
        ["V", "-", "-", "=", "=", "=", "N", "=", "-", "C", "=", "-", "T", "=", "-"],
    ]
    for g in groups:
        ks, f = seq(f, 25, g)
        k += ks
    # second field: C/N/T again within key_recent
    for g in (["C", "C", "=", "-"], ["N", "N", "-", "="], ["T", "T", "=", "T", "=", "T", "=", "T", "-"]):
        ks, f = seq(f, 6, g)
        k += ks
        f += 30
    # held keys: radio and OBI wrap-arounds, elevator acceleration, trim, rudder
    k += [f"{f}:C*2", f"{f + 10}:=*400"]
    f += 440
    k += [f"{f}:N*2", f"{f + 10}:-*400"]
    f += 440
    k += [f"{f}:V*2", f"{f + 10}:-*300", f"{f + 320}:=*300"]
    f += 640
    for key in ["Keypad 8", "Keypad 2", "Keypad 4", "Keypad 6", "Keypad 0", "Keypad +", "Keypad 7", "Keypad 1"]:
        k.append(f"{f}:{key}*90")
        f += 110
    # radar view: zoom both ways, off; view directions after Scroll Lock
    ks, f = seq(f, 25, ["Numlock", "-", "-", "-", "=", "=", "=", "=", "Keypad 4", "ScrollLock"])
    k += ks
    for key in KEYPAD:
        ks, f = seq(f, 20, ["ScrollLock", key])
        k += ks
    ks, f = seq(f, 25, ["P", "Keypad 8", "P", "Keypad *", "F2"])
    k += ks
    return "keys_flight", ["--keys", ",".join(k)], f + 600


def session_ground():
    """On the runway: brakes while rolling, gear on the ground, full throttle in reality mode."""
    k = list(START)
    ks, f = editor_set(300, 4, 1)  # reality mode on
    k += ks
    for d in range(0, 2400, 120):
        k.append(f"{f + d}:F2*2")  # F2 checks frame_counter & 3 and the throttle (engine_stop)
        k.append(f"{f + d + 40}:F10*2")
    f += 2400
    k += [f"{f}:F2*2", f"{f + 400}:.*120", f"{f + 600}:G*2", f"{f + 700}:Keypad 5*2", f"{f + 800}:.*2"]
    return "keys_ground", ["--keys", ",".join(k)], f + 1200


def session_slew():
    k = list(START)
    ks, f = editor_set(300, 3, 1)
    k += ks
    ks, f = seq(f + 60, 30, KEYPAD + ["Keypad 0", "Keypad +", "Keypad 5", "Left Alt", "T", "Keypad 9",
                                      "Keypad 3", "T", "F2", "F10", "F5", "Keypad 8"], hold=20)
    k += ks
    ks, f = seq(f, 20, ["ScrollLock", "Keypad 6", "Left Alt", "Keypad 5", "Numlock", "-", "=", "ScrollLock"])
    k += ks
    return "keys_slew", ["--keys", ",".join(k)], f + 300


def session_war():
    k = list(START)
    ks, f = editor_set(300, 6, 1)
    k += ks
    k += [f"{f}:F2*2"]
    ks, f = seq(f + 600, 30, ["W", "W", "W", "W", "X", "X", "X", "X", "X", "X", "X", "-", "-", "=", "Space",
                              "Space", "A", "D", "Numlock", "-", "=", "ScrollLock", "Keypad *"], hold=6)
    k += ks
    return "keys_war", ["--keys", ",".join(k)], f + 1500


def session_editor():
    k = list(START)
    f = 300
    k.append(f"{f}:Escape*2")
    f += 40
    ks, f = seq(f, 20, ["Return", "Return", "Return", "1", "2", "Backspace", "Backspace", "Backspace", "-", "-",
                        "=", "Return", "Return", "Return", "7", "Return", "-", "Keypad *", "Insert", "Return",
                        "9", "9", "Return", "Keypad *", "Escape"])
    k += ks
    ks, f = seq(f + 120, 20, ["Escape", "Return", "Return", "4", "Return", "Escape"])
    k += ks
    return "keys_editor", ["--keys", ",".join(k)], f + 600


def session_reboot():
    k = START + ["400:F2*2", "450:Keypad 8*2", "500:Left Ctrl*60", "510:Left Alt*50", "520:Delete*2",
                 "800:C", "860:B", "1000:F4*2"]
    return "keys_reboot", ["--keys", ",".join(k)], 1400


def sessions():
    return [session_flight(), session_ground(), session_slew(), session_war(), session_editor(), session_reboot()]


def key_natives():
    r = subprocess.run([EXE, "--list-natives"], cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True)
    out = []
    for line in (r.stdout + r.stderr).splitlines():
        m = re.match(r"(\S+)\s+0050:([0-9A-F]{4})\s", line)
        if m and 0x0955 <= int(m.group(2), 16) <= 0x10B5:
            out.append(m.group(1))
    return out


def verify(session, names, extra):
    name, args, frames = session
    cmd = [EXE, "--frames", str(frames)]
    for n in names:
        skip = NO_VERIFY["reboot"] if name.endswith("reboot") else ()
        cmd += ["--native-on", n] + (["--verify", n] if n not in skip else [])
    r = subprocess.run(cmd + extra + args, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True, timeout=3600)
    out = r.stdout + r.stderr
    res = {m.group(1): (int(m.group(2)), int(m.group(3))) for m in SUMMARY.finditer(out)}
    return name, r.returncode, res, [l for l in out.splitlines() if l.startswith("verify ")]


def shots(session, off, tmp):
    name, args, frames = session
    tag = "off" if off else "on"
    base = os.path.join(tmp, f"{name}_{tag}")
    cmd = [EXE, "--frames", str(frames), "--screenshot", base + ".bmp"]
    for f in range(300, frames, 300):
        cmd += ["--shot-at", str(f)]
    for n in off:
        cmd += ["--native-off", n]
    r = subprocess.run(cmd + args, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True, timeout=3600)
    return r.returncode


def main():
    argv = sys.argv[1:]
    if argv[:1] == ["--shots"]:
        names = key_natives()
        with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            all_s = sessions() + campaign_sessions()
            jobs = [(s, off) for s in all_s for off in ([], names)]
            list(pool.map(lambda j: shots(j[0], j[1], tmp), jobs))

            def differing():
                out, n = [], 0
                for f in sorted(os.listdir(tmp)):
                    if "_on" not in f:
                        continue
                    n += 1
                    other = os.path.join(tmp, f.replace("_on", "_off"))
                    if not os.path.exists(other) or not filecmp.cmp(os.path.join(tmp, f), other, shallow=False):
                        out.append(f)
                return out, n

            diff, total = differing()
            # Text mode blinks by wall-clock time (main.c: blink_phase from SDL_GetTicks), so a
            # menu screenshot can differ under load. Re-run such sessions once, one at a time.
            again = {f.split("_on")[0] for f in diff}
            for s in all_s:
                if s[0] in again:
                    print("re-running", s[0])
                    shots(s, [], tmp)
                    shots(s, names, tmp)
            diff, total = differing()
            bad = len(diff)
            for f in diff:
                print("differs:", f)
        print(f"{total} screenshot pairs, {bad} differ")
        print("FAIL" if bad or not total else "OK")
        sys.exit(1 if bad or not total else 0)
    extra = []
    if "--" in argv:
        i = argv.index("--")
        argv, extra = argv[:i], argv[i + 1:]
    names = argv[0].split(",") if argv else key_natives()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        done = list(pool.map(lambda s: verify(s, names, extra), sessions()))
    failed = False
    print(f"{'native':24s} " + " ".join(f"{s[0]:>14s}" for s in done) + "      total")
    for n in names:
        cells, tc, tm = [], 0, 0
        for _, _, res, _ in done:
            c, m = res.get(n, (0, 0))
            tc, tm = tc + c, tm + m
            cells.append(f"{c:>9d}/{m:<4d}")
        failed |= tm > 0
        print(f"{n:24s} " + " ".join(cells) + f" {tc:>7d}/{tm}")
    for sname, code, res, reports in done:
        failed |= code != 0 or not res
        if not res:
            print(f"{sname}: no verify summary (exit {code})")
        for line in reports[:5]:
            print(f"{sname}: {line}")
    print("(calls/mismatches per session)")
    print("FAIL" if failed else "OK")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
