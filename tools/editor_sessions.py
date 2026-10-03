"""Editor, preset and start-up menu sessions for subphase 3.16; see docs/subphases/3.16.md.

usage:
  python tools/editor_sessions.py list
  python tools/editor_sessions.py verify [NAMES]     --verify NAMES (default: every native that
                                                     returns, i.e. all but editor_key_backspace,
                                                     preset_save and preset_load)
  python tools/editor_sessions.py ab                 natives on vs the 3.16 natives off: compare
                                                     screenshots, the final memory dump and CPU state
  python tools/editor_sessions.py shots TAG [ARGS]   screenshots only, to extracted/editor/TAG_*
  python tools/editor_sessions.py persist            presets saved in one run, loaded in the next
  env ONLY=prefix selects sessions by name; CAMPAIGN=1 uses the trace campaign's sessions instead.

Each run gets its own temporary working directory (for extracted/mem_dump.bin) and, for the
preset sessions, its own FS1_PRESETS file. Keys: Esc enters / leaves the editor, Return
moves down a row, '-' up, digits type, Backspace deletes, 'Keypad *' recalls the user mode,
Insert stores it, 's' / 'l' save / load all presets. Editor rows: page 1 has rows 2..44
(user mode 2, ..., North 22, East 24, Altitude 26, Pitch 28, Bank 30, Heading 32, Airspeed
34, Throttle 36); the field number of trace_campaign.editor_set() is (row - 2) / 2.
"""
import filecmp
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from trace_campaign import EXE, HEADLESS_ENV  # noqa: E402

OUT = os.path.join(ROOT, "extracted", "editor")
IMAGE = os.path.join(ROOT, "original", "Microsoft Flight Simulator v1.05 (198x)(Microsoft Corporation).ima")

MINE = ["editor_row_ptr", "editor_cursor_clear", "editor_first_page", "editor_next_row", "editor_prev_row",
        "editor_page_text", "editor_draw_page", "editor_field_next", "editor_draw_fields", "editor_read_fields",
        "editor_commit", "usermode_store", "usermode_recall", "editor_key_accept", "editor_key_up",
        "editor_key_star", "editor_key_ins", "editor_key_backspace", "preset_save", "preset_load",
        "editor_capture_state", "speaker_off", "editor_scale_values", "editor_scale_wind", "editor_apply_state",
        "alt_from_editor", "check_master_disk"]
NO_RETURN = {"editor_key_backspace", "preset_save", "preset_load"}


class Script:
    """Key script: menus, then keys at a frame cursor (20 frames apart, held 2 frames)."""

    def __init__(self, display="A", mode="B"):
        self.keys = [f"120:{display}", f"180:{mode}"]
        self.f = 300
        self.shots = []

    def key(self, name, gap=20, hold=2):
        self.keys.append(f"{self.f}:{name}*{hold}")
        self.f += gap
        return self

    def editor(self):  # Esc: enter or leave the editor
        return self.key("Escape", 40)

    def type(self, text):
        for ch in text:
            self.key(ch)
        return self

    def enter(self, n=1):
        for _ in range(n):
            self.key("Return")
        return self

    def up(self, n):
        for _ in range(n):
            self.key("-")
        return self

    def shot(self, wait=10):
        self.f += wait
        self.shots.append(self.f)
        self.f += 10
        return self

    def session(self, name, tail=600, preset=None, ab=True):
        return {"name": name, "keys": self.keys, "frames": self.f + tail, "shots": self.shots + [self.f + tail - 1],
                "preset": preset, "ab": ab}


PAGE1 = ["", "0", "1", "1", "1", "0", "0", "5", "7", "", "17500", "21000", "1500", "5", "10", "90", "100",
         "20000", "32767", "32767", "0", "32767"]
PAGE2 = ["14", "30", "2", "5000", "4000", "3000", "2000", "15", "270", "8000", "10", "180", "6000", "5", "90",
         "4000", "3", "45", "", "4", "", ""]


def sessions():
    out = []
    # every row of page 1 and page 2 typed into, then fly
    s = Script().editor().shot()
    for v in PAGE1:
        s.type(v).enter()
    s.shot()
    for v in PAGE2:
        s.type(v).enter()
    s.shot().editor().shot(60)
    out.append(s.session("all_fields", tail=900))
    # every row walked without typing (cursor, page flips both ways)
    s = Script().editor().enter(22).shot().enter(22).shot().enter(3).shot().editor()
    out.append(s.session("walk_pages"))
    # Backspace: more presses than digits, then typing again
    s = Script().editor().enter(10).type("123").key("Backspace").key("Backspace").type("45").shot()
    s.key("Backspace").key("Backspace").key("Backspace").key("Backspace").type("9").shot().enter().editor()
    out.append(s.session("backspace"))
    # '-': up from row 4 to 2, up again (other page, row 2Ch), up after typing (commit, then up)
    s = Script().editor().enter(1).key("-").shot().key("-").shot().key("-").type("12").key("-").shot()
    s.key("-").key("-").shot().editor()
    out.append(s.session("up_keys"))
    # '=' accepts like Return
    s = Script().editor().enter(12).type("2500").key("=").shot().editor()
    out.append(s.session("equals_key"))
    # user modes 0..9 (recall), the same mode again (store to n+10), 110+n (store to n+10)
    s = Script().editor()
    for m in range(10):
        s.type(str(m)).enter().shot(30)  # the cursor stays on the User mode row
    s.type("4").enter().type("4").enter().shot(30)
    s.type("112").enter().shot(30).type("12").enter().shot(30).editor()
    out.append(s.session("user_modes", tail=600))
    for m in (0, 6, 7, 8, 9):
        s = Script().editor().type(str(m)).enter().editor()
        out.append(s.session(f"fly_mode_{m}", tail=900))
    # '*' (recall the current mode) and Insert (store; with mode < 10: as typed)
    s = Script().editor().type("3").enter().enter(12).type("4000").enter().shot().key("Keypad *").shot()
    s.enter(2).type("200").enter().key("Insert").shot()
    s.up(17).type("113").enter().shot().enter(13).type("150").enter().key("Insert").shot()
    s.key("Keypad *").shot().editor()
    out.append(s.session("star_insert", tail=900))
    # s / l: save, change slot 12 (store with 112), load, recall 12 = the saved values
    s = Script().editor().key("s", 60).shot().enter(13).type("45").enter().up(14)
    s.type("112").enter().shot().key("l", 60).shot().type("3").enter().type("12").enter().shot().editor()
    out.append(s.session("save_load", tail=600, preset="new"))
    # l without a preset file (unformatted track): the presets become the text screen
    s = Script().editor().key("l", 60).shot().type("3").enter().shot().editor()
    out.append(s.session("load_no_file", tail=600, preset="none", ab=False))
    # wind level 3 knots of 30000: DIV overflow (INT 0) in editor_apply_state
    s = Script().editor().enter(22 + 7).type("30000").enter().shot().editor()
    out.append(s.session("wind_overflow", tail=900))
    # pitch / bank of 3 digits (3.5 reported Esc not leaving the editor)
    for fld, row in (("pitch", 13), ("bank", 14)):
        s = Script().editor().enter(row).type("123").enter().shot().editor().shot(60)
        out.append(s.session(f"{fld}_123", tail=900))
    # start-up menus: three displays, demo (A) and flight (B)
    for d in "ABC":
        for m in "AB":
            s = Script(d, m)
            s.f = 400
            s.key("F2")
            out.append(s.session(f"menu_{d}{m}", tail=900))
    return out


def run(sess, extra, tag=None, preset_file=None):
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "extracted"))
        env = dict(HEADLESS_ENV)
        if sess["preset"]:
            env["FS1_PRESETS"] = preset_file or os.path.join(tmp, "presets.bin")
        cmd = [EXE, IMAGE, "--frames", str(sess["frames"]), "--keys", ",".join(sess["keys"]), "--dump-on-exit"]
        shots_base = os.path.join(OUT, f"{tag}_{sess['name']}") if tag else os.path.join(tmp, "shot")
        cmd += ["--screenshot", shots_base]
        for f in sess["shots"]:
            cmd += ["--shot-at", str(f)]
        cmd += extra
        r = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True, timeout=3600)
        out = r.stdout + r.stderr
        dump = open(os.path.join(tmp, "extracted", "mem_dump.bin"), "rb").read()
        shots = {}
        for f in sess["shots"]:
            p = f"{shots_base}_{f}.bmp"
            shots[f] = open(p, "rb").read() if os.path.exists(p) else None
        stop = next((l for l in out.splitlines() if "Stopped at" in l), "")
        return out, dump, shots, stop


def cmd_verify(names):
    extra = []
    for n in names:
        extra += ["--native-on", n, "--verify", n]
    ss = selected()
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as p:
        results = list(p.map(lambda s: (s["name"], run(s, extra)[0]), ss))
    total = {}
    bad = False
    for name, out in results:
        summ = re.findall(r"verify-summary (\S+) calls (\d+) mismatches (\d+)", out)
        reports = [l for l in out.splitlines() if l.startswith("verify ")]
        calls = {n: int(c) for n, c, m in summ if n in MINE and int(c)}
        for n, c, m in summ:
            t = total.setdefault(n, [0, 0])
            t[0] += int(c)
            t[1] += int(m)
            bad |= int(m) > 0
        print(f"{name:16s} " + " ".join(f"{n}={c}" for n, c in sorted(calls.items())))
        for l in reports[:5]:
            print("   ", l)
    print("\nTOTAL (3.16 natives)")
    for n in MINE:
        if n in total:
            print(f"  {n:22s} calls {total[n][0]:6d} mismatches {total[n][1]}")
    others = sum(t[1] for n, t in total.items() if n not in MINE)
    print(f"  other natives: mismatches {others}")
    print("\nFAIL" if bad else "\nOK")
    return 1 if bad else 0


def cmd_ab():
    off = []
    for n in MINE:
        off += ["--native-off", n]
    ss = [s for s in selected() if s["ab"]]
    bad = False

    def pair(s):
        with tempfile.TemporaryDirectory() as tmp:
            pa = os.path.join(tmp, "a.bin")
            pb = os.path.join(tmp, "b.bin")
            a = run(s, [], preset_file=pa)
            b = run(s, off, preset_file=pb)
            return s, a, b

    with ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 2) // 2)) as p:
        for s, a, b in p.map(pair, ss):
            same_shots = [f for f in s["shots"] if a[2][f] is not None and a[2][f] == b[2][f]]
            alld = [i for i in range(len(a[1])) if a[1][i] != b[1][i]]
            # The stack is 0000:0000-03FE; the dumps are taken in main_loop (SP about 03FC), so
            # 0300-03EF is below SP: interrupt frames and return addresses left by calls. A native
            # holds interrupts until it returns, so their frames land at other depths there.
            stack = [i for i in alld if 0x300 <= i < 0x3F0]
            diff = [i for i in alld if not 0x300 <= i < 0x3F0]
            ok = len(same_shots) == len(s["shots"]) and a[3] == b[3] and not diff
            bad |= not ok
            print(f"{s['name']:16s} shots {len(same_shots)}/{len(s['shots'])} identical, "
                  f"cpu {'same' if a[3] == b[3] else 'DIFFERENT'}, memory bytes differing {len(diff)}"
                  + (f" (+{len(stack)} below SP)" if stack else "") + ("" if ok else "  <--"))
            if diff:
                print("    first differing addresses:", " ".join(f"{d:05X}" for d in diff[:12]))
            if a[3] != b[3]:
                print("    on: ", a[3][-120:], "\n    off:", b[3][-120:])
    print("\nFAIL" if bad else "\nOK")
    return 1 if bad else 0


def cmd_shots(tag, extra):
    os.makedirs(OUT, exist_ok=True)
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as p:
        for s, (out, _, shots, stop) in p.map(lambda s: (s, run(s, extra, tag)), selected()):
            print(f"{s['name']:16s} {len([v for v in shots.values() if v])} shots  {stop[:70]}")
            checks = [l for l in out.splitlines() if l.startswith("cycle-check")]
            for l in checks[:6]:
                print("   ", l)
            if len(checks) > 6:
                print(f"    ... {len(checks)} cycle-check lines")


def campaign_sessions():
    """The trace campaign's sessions (tools/trace_campaign.py), with a screenshot every 300 frames."""
    from trace_campaign import sessions as campaign
    out = []
    for name, args, frames in campaign():
        keys = args[args.index("--keys") + 1].split(",")
        out.append({"name": "campaign_" + name, "keys": keys, "frames": frames,
                    "shots": list(range(300, frames, 300)) + [frames - 1], "preset": None, "ab": True})
    return out


def cmd_persist():
    """Presets across restarts: run 1 stores altitude 4321 in user mode 12 and saves ('s'),
    run 2 (a new process) loads ('l') and recalls mode 12."""
    with tempfile.TemporaryDirectory() as tmp:
        pf = os.path.join(tmp, "presets.bin")
        s1 = Script().editor().enter(12).type("4321").enter().up(13).type("112").enter().key("s", 60)
        s2 = Script().editor().key("l", 60).type("3").enter().type("12").enter()
        run(s1.session("persist_save", tail=60, preset="file"), [], preset_file=pf)
        size = os.path.getsize(pf) if os.path.exists(pf) else 0
        _, dump, _, _ = run(s2.session("persist_load", tail=60, preset="file"), [], preset_file=pf)
        ds = 0x6180
        alt = dump[ds + 0x2054] | dump[ds + 0x2055] << 8
        ok = size == 0x79E and alt == 4321 and dump[ds + 0x2048] == 12
        print(f"preset file {size} bytes; after load and recall of mode 12: user mode {dump[ds + 0x2048]}, "
              f"altitude {alt}\n\n{'OK' if ok else 'FAIL'}")
        return 0 if ok else 1


def selected():
    only = os.environ.get("ONLY")
    pool = campaign_sessions() if os.environ.get("CAMPAIGN") else sessions()
    return [s for s in pool if not only or s["name"].startswith(only)]


def main():
    a = sys.argv[1:]
    if not a or a[0] == "list":
        for s in sessions():
            print(f"{s['name']:16s} frames {s['frames']:6d} shots {len(s['shots'])}  {','.join(s['keys'])[:100]}")
        return 0
    if a[0] == "verify":
        if len(a) > 1:
            names = a[1].split(",")
        else:
            listing = subprocess.run([EXE, "--list-natives"], env=HEADLESS_ENV, capture_output=True, text=True)
            names = [l.split()[0] for l in (listing.stdout + listing.stderr).splitlines()
                     if re.match(r"^\S+\s+0050:[0-9A-F]{4}", l)]
            names = [n for n in names if n not in NO_RETURN and n != "test_passthrough"]
        return cmd_verify(names)
    if a[0] == "persist":
        return cmd_persist()
    if a[0] == "ab":
        return cmd_ab()
    if a[0] == "shots":
        return cmd_shots(a[1], a[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
