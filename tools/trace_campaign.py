"""Run scripted sessions of the original game under the harness and accumulate an
execution/data trace map (see cpu8086.h T_* flags) in extracted/trace.bin.

usage: python tools/trace_campaign.py [--fresh]

Each session boots the game, picks menu options and presses keys at given frames
(60 frames = 1 s of emulated time). Sessions are chosen to reach as much of the program
as possible: demo flight, every flight key, the editor, slew, war mode, other
user-mode start positions, all display types and a Ctrl+Alt+Del reboot.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# These tools run original code, so they need the FS1_EMULATOR build (3.22): build-emu/ by
# default (cmake -B build-emu -DFS1_EMULATOR=ON), or the executable named by FS1_EXE.
EXE = os.environ.get("FS1_EXE") or os.path.join(ROOT, "build-emu", "fs1-enhanced.exe" if os.name == "nt" else "fs1-enhanced")
# Scripted runs never need a visible window or sound; this also makes them work on
# headless Linux machines (cloud sessions, CI). Set FS1_SHOW=1 to watch them.
HEADLESS_ENV = dict(os.environ)
if not os.environ.get("FS1_SHOW"):
    HEADLESS_ENV.setdefault("SDL_VIDEODRIVER", "offscreen")
    HEADLESS_ENV.setdefault("SDL_AUDIODRIVER", "dummy")
TRACE = os.path.join(ROOT, "extracted", "trace.bin")


def keys_every(start, step, names, hold=2):
    out, f = [], start
    for n in names:
        out.append(f"{f}:{n}*{hold}")
        f += step
    return out, f


def editor_set(start, field, value):
    """Esc, move down `field` fields with Enter, type value, Enter, Esc."""
    out, f = [f"{start}:Escape*2"], start + 40
    for _ in range(field):
        out.append(f"{f}:Return*2")
        f += 20
    for ch in str(value):
        out.append(f"{f}:{ch}*2")
        f += 20
    out += [f"{f}:Return*2", f"{f + 40}:Escape*2"]
    return out, f + 100


def session_flight_keys():
    k = ["120:A", "180:B"]
    f = 300
    groups = [
        ["F2", "F4", "F6", "F8", "F10", "F1", "F3", "F5", "F7", "F9", "F2"],
        ["Keypad 8", "Keypad 2", "Keypad 4", "Keypad 6", "Keypad 0", "Keypad +", "Keypad 5",
         "Keypad 7", "Keypad 1", "Keypad 9", "Keypad 3"],
        ["1", "2", "3", "5", "4", "0", "4", "G", "H", "H", "L", "L", "D", "M", "."],
        ["C", "=", "-", "C", "=", "N", "=", "-", "N", "=", "V", "=", "-", "T", "=", "T", "=", "T", "=", "T", "="],
        ["Numlock", "=", "=", "-", "ScrollLock", "Tab", "Tab", "P", "P", "Keypad *", "W", "A", "X", "Space"],
    ]
    for g in groups:
        ks, f = keys_every(f, 25, g)
        k += ks
    # take-off attempt: full throttle, roll, rotate
    k += [f"{f}:F2*2", f"{f + 1500}:Keypad 2*2", f"{f + 1530}:Keypad 2*2", f"{f + 2400}:G*2"]
    return "flight_keys", ["--keys", ",".join(k)], f + 3200


def session_demo():
    return "demo", ["--keys", "120:A,180:A"], 6000


def session_display(choice):
    return f"display_{choice}", ["--keys", f"120:{choice},180:B,400:F2*2"], 1200


def session_slew():
    k = ["120:A", "180:B"]
    ks, f = editor_set(300, 3, 1)
    k += ks
    ks, f = keys_every(f + 60, 30, ["Keypad 8", "Keypad 2", "Keypad 4", "Keypad 6", "Keypad 7", "Keypad 1",
                                    "Keypad 9", "Keypad 3", "Keypad 0", "Keypad +", "Keypad 5", "Left Alt",
                                    "F2", "F10", "Keypad 8"], hold=20)
    k += ks
    return "slew", ["--keys", ",".join(k)], f + 300


def session_war():
    k = ["120:A", "180:B"]
    ks, f = editor_set(300, 6, 1)
    k += ks
    k += [f"{f}:F2*2"]
    ks, f = keys_every(f + 600, 40, ["Space", "Space", "W", "W", "W", "X", "A", "Space", "Keypad 4", "Keypad 6",
                                     "Space", "X"], hold=10)
    k += ks
    return "war", ["--keys", ",".join(k)], f + 3000


def session_user_mode(mode):
    k = ["120:A", "180:B"]
    ks, f = editor_set(300, 0, mode)
    k += ks + [f"{f}:F2*2"]
    return f"user_mode_{mode}", ["--keys", ",".join(k)], f + 1500


def session_editor_values():
    """Edit position/altitude fields on page 1 and flip to other pages."""
    k = ["120:A", "180:B"]
    ks, f = editor_set(300, 12, 3000)  # altitude (fields: 10 North, 11 East, 12 Altitude, 16 Airspeed)
    k += ks
    ks, f = editor_set(f, 16, 120)  # airspeed
    k += ks
    ks, f = editor_set(f, 4, 1)  # reality mode
    k += ks
    ks, f = editor_set(f, 1, 0)  # sound off
    k += ks
    return "editor_values", ["--keys", ",".join(k)], f + 1500


def session_reboot():
    return "reboot", ["--keys", "120:A,180:B,400:Left Ctrl*60,410:Left Alt*50,420:Delete*2,700:C,760:B"], 1200


def sessions():
    """All campaign sessions as (name, extra_args, frames); also used by verify_campaign.py."""
    out = [session_demo(), session_flight_keys(), session_slew(), session_war(),
           session_editor_values(), session_reboot()]
    out += [session_display(c) for c in "ABC"]
    out += [session_user_mode(m) for m in range(1, 6)]
    return out


def main():
    os.makedirs(os.path.dirname(TRACE), exist_ok=True)
    if "--fresh" in sys.argv and os.path.exists(TRACE):
        os.remove(TRACE)
    for name, args, frames in sessions():
        cmd = [EXE, "--frames", str(frames), "--trace", TRACE] + args
        r = subprocess.run(cmd, cwd=ROOT, env=HEADLESS_ENV, capture_output=True, text=True, timeout=1800)
        notes = [l for l in (r.stdout + r.stderr).splitlines() if "unknown" in l or "unhandled" in l]
        print(f"{name:16s} {frames:6d} frames  exit {r.returncode}", *notes)
    data = open(TRACE, "rb").read()
    print("instruction starts:", sum(1 for b in data if b & 1),
          " call targets:", sum(1 for b in data if b & 2),
          " SMC bytes:", sum(1 for b in data if b & 0x40))


if __name__ == "__main__":
    main()
