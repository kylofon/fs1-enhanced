"""Sessions that move the aircraft through all five scenery areas, for subphase 3.20 (scenery
loading); see docs/subphases/3.20.md.

usage:
  python tools/scenery_sessions.py list
  python tools/scenery_sessions.py verify          the three --verify configurations below
  python tools/scenery_sessions.py ab              natives on vs the 3.20 natives off: screenshots,
                                                   final memory dump and CPU state must be identical
  python tools/scenery_sessions.py shots TAG [ARGS]  screenshots only, to extracted/scenery/TAG_*

The editor's North / East fields (rows 22 / 24, trace_campaign field numbers 10 / 11) show the
position + 16384 (integer units of 256 m), so area i is reached by typing the editor values in
TOUR. Each move sets North, East and Altitude in one editor visit (Esc, 10 x Return, digits,
Return, digits, Return, digits, Esc), so the position jumps straight into the next area. A
screenshot is taken TOUR_WAIT frames after leaving the editor, when the load is long over.

Sessions:
  scenery_flight   flight mode (menus A, B), tour areas 1, 2, 3, 4, 0 at 1000 altitude units,
                   full throttle
  scenery_slew     the same tour in slew mode (editor field 3 = 1 first)
  scenery_ground   the tour at altitude 0 (on the ground), areas 3, 1, 4, 2, 0

--verify configurations (verify):
  all        every native on; the 3.20 natives verified. A verified select_scenery_area runs a
             whole load (the original single-stepped), so the inner entries are not reached.
  loop       select_scenery_area off: the original reaches 044E, scenery_load_loop is verified
             (each call is a whole load from the first byte of the copy loop).
  int13      select_scenery_area and scenery_load_loop off: the INT 13h natives are verified at
             every disk reset and track read of a load.
"""
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from trace_campaign import EXE, HEADLESS_ENV  # noqa: E402

OUT = os.path.join(ROOT, "extracted", "scenery")
IMAGE = os.path.join(ROOT, "original", "Microsoft Flight Simulator v1.05 (198x)(Microsoft Corporation).ima")

MINE = ["select_scenery_area", "scenery_load_loop", "disk_reset_int13", "read_track_int13"]
OFFSET = 16384
# area: (north, east) inside area_bounds (DS:0449), see docs/SCENERY_FORMAT.md
AREAS = {0: (786, 287), 1: (-1500, -10500), 2: (5500, -10500), 3: (1000, 5500), 4: (1500, -7700)}
TOUR_WAIT = 400


class Script:
    def __init__(self):
        self.keys = ["120:A", "180:B"]
        self.f = 300
        self.shots = []

    def key(self, name, gap=20, hold=2):
        self.keys.append(f"{self.f}:{name}*{hold}")
        self.f += gap
        return self

    def type(self, text):
        for ch in str(text):
            self.key(ch)
        return self

    def editor_fields(self, first, values):
        """Esc, Return to field `first`, then each value followed by Return, Esc."""
        self.key("Escape", 40)
        for _ in range(first):
            self.key("Return")
        for v in values:
            self.type(v).key("Return")
        self.key("Escape", 40)
        return self

    def move(self, area, alt):
        n, e = AREAS[area]
        self.editor_fields(10, [n + OFFSET, e + OFFSET, alt])
        self.f += TOUR_WAIT
        self.shots.append(self.f)
        self.f += 10
        return self

    def session(self, name):
        return {"name": name, "keys": self.keys, "frames": self.f + 60, "shots": list(self.shots)}


def sessions():
    out = []
    s = Script()
    s.key("F2", 20, 30)  # full throttle
    for a in (1, 2, 3, 4, 0):
        s.move(a, 1000)
    out.append(s.session("scenery_flight"))
    s = Script()
    s.editor_fields(3, [1])  # slew on
    for a in (1, 2, 3, 4, 0):
        s.move(a, 1000)
    out.append(s.session("scenery_slew"))
    s = Script()
    for a in (3, 1, 4, 2, 0):
        s.move(a, 0)
    out.append(s.session("scenery_ground"))
    return out


def run(sess, extra, tag=None):
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "extracted"))
        cmd = [EXE, IMAGE, "--frames", str(sess["frames"]), "--keys", ",".join(sess["keys"]), "--dump-on-exit"]
        if tag:
            os.makedirs(OUT, exist_ok=True)
        shots_base = os.path.join(OUT, f"{tag}_{sess['name']}") if tag else os.path.join(tmp, "shot")
        cmd += ["--screenshot", shots_base]
        for f in sess["shots"]:
            cmd += ["--shot-at", str(f)]
        cmd += extra
        r = subprocess.run(cmd, cwd=tmp, env=HEADLESS_ENV, capture_output=True, text=True, timeout=7200)
        out = r.stdout + r.stderr
        p = os.path.join(tmp, "extracted", "mem_dump.bin")
        dump = open(p, "rb").read() if os.path.exists(p) else b""
        shots = {}
        for f in sess["shots"]:
            p = f"{shots_base}_{f}.bmp"
            shots[f] = open(p, "rb").read() if os.path.exists(p) else None
        stop = next((l for l in out.splitlines() if "Stopped at" in l), "")
        return out, dump, shots, stop


def off_args(names):
    return [a for n in names for a in ("--native-off", n)]


CONFIGS = {
    "all": ([], MINE),
    "loop": (off_args(["select_scenery_area"]), ["scenery_load_loop"]),
    "int13": (off_args(["select_scenery_area", "scenery_load_loop"]), ["disk_reset_int13", "read_track_int13"]),
}


def cmd_verify():
    jobs = []
    for cname, (base, names) in CONFIGS.items():
        extra = base + [a for n in names for a in ("--verify", n)]
        for s in sessions():
            jobs.append((cname, s, extra, names))
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as p:
        results = list(p.map(lambda j: (j[0], j[1]["name"], j[3], run(j[1], j[2])[0]), jobs))
    bad = False
    for cname, sname, names, out in results:
        summ = {n: (int(c), int(m)) for n, c, m in re.findall(r"verify-summary (\S+) calls (\d+) mismatches (\d+)",
                                                               out)}
        cyc = dict(re.findall(r"verify-summary (\S+) calls \d+ mismatches \d+ original-cycles (\S+)", out))
        line = " ".join(f"{n}={summ.get(n, (0, 0))[0]}/{summ.get(n, (0, 0))[1]} ({cyc.get(n, '-')})" for n in names)
        print(f"{cname:6s} {sname:16s} calls/mismatches: {line}")
        for l in [l for l in out.splitlines() if l.startswith("verify ")][:5]:
            print("   ", l)
        bad |= not summ or any(m for c, m in summ.values())
    print("FAIL" if bad else "OK")
    return 1 if bad else 0


def cmd_ab():
    ss = sessions()
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as p:
        on = list(p.map(lambda s: run(s, []), ss))
        off = list(p.map(lambda s: run(s, off_args(MINE)), ss))
    bad = False
    for s, a, b in zip(ss, on, off):
        same_shots = all(a[2][f] is not None and a[2][f] == b[2][f] for f in s["shots"])
        diff = [i for i in range(min(len(a[1]), len(b[1]))) if a[1][i] != b[1][i]]
        ok = same_shots and not diff and a[3] == b[3] and len(a[1]) == len(b[1]) and a[1]
        bad |= not ok
        print(f"{s['name']:16s} shots {'same' if same_shots else 'DIFFER'} ({len(s['shots'])}), "
              f"memory {'same' if not diff else f'{len(diff)} bytes differ, first {diff[0]:05X}'}, "
              f"state {'same' if a[3] == b[3] else 'DIFFER'}")
        if a[3] != b[3]:
            print("   on: ", a[3])
            print("   off:", b[3])
    print("FAIL" if bad else "OK")
    return 1 if bad else 0


def cmd_shots(tag, extra):
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as p:
        list(p.map(lambda s: run(s, extra, tag), sessions()))
    print(f"screenshots in {OUT}")
    return 0


def main():
    argv = sys.argv[1:]
    cmd = argv[0] if argv else "list"
    if cmd == "list":
        for s in sessions():
            print(f"{s['name']:16s} {s['frames']:6d} frames, shots at {s['shots']}")
        return 0
    if cmd == "verify":
        return cmd_verify()
    if cmd == "ab":
        return cmd_ab()
    if cmd == "shots":
        return cmd_shots(argv[1], argv[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
