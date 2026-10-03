"""Render the speaker audio of campaign sessions with natives on and off, and compare.

usage: python tools/audio_compare.py [NAMES] [--sessions demo,flight_keys,war] [--wav DIR] [--keep DIR] [--csched]

Builds a variant of fs1 (build/fs1_audio.exe) from the sources unchanged; only main.c is
compiled with its SDL audio stream calls and its pc_speaker_render call renamed (-D) to shim
functions, which
  - log every speaker event of the slice (cycle, port 61h, PIT channel 2 reload) to the file
    named by FS1_EVENTS_OUT, then call the real pc_speaker_render;
  - append the rendered samples (44.1 kHz mono float) to the file named by FS1_AUDIO_OUT and
    always accept them, so the output does not depend on how fast a device drains its queue.
Each session runs twice: with all natives on, and with NAMES (comma separated, default: the
3.19 sound natives) off. The speaker events, the audio rendered from them (pc_speaker_render's
model on one global sample grid) and the final screenshot must be identical; the tool exits 1
on any difference. The audio as played is compared too, for information: main.c renders it per
frame, and the sound natives are handed back when they span the end of a frame (S3F.2), so it
is identical as well. --wav DIR also writes both renderings as 16-bit WAV files for listening;
--keep DIR keeps the raw files there.
--csched (3.22) compares the emulated PC (all natives on) with the C scheduler instead of natives
on/off ("off" is then the C-scheduler run). The two are not expected to be identical: with the
C scheduler interrupts come between natives, so edges move by up to one native step; the tool
prints how far the event streams are apart.

Needs gcc and SDL3 (MSYS2 MinGW on Windows: pkg-config sdl3) and the disk image in original/.
"""
import argparse
import array
import math
import os
import shutil
import subprocess
import sys
import tempfile
import wave
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_campaign import HEADLESS_ENV, ROOT, sessions  # noqa: E402

EXE = os.path.join(ROOT, "build", "fs1_audio.exe" if os.name == "nt" else "fs1_audio")
SOUND_NATIVES = ["int8_timer", "sound_update", "speaker_tone"]

SHIM = r"""
#include <stdio.h>
#include <stdlib.h>
#include <SDL3/SDL.h>
#include "pc.h"

static FILE *open_env(const char *name, const char *mode)
{
    const char *p = getenv(name);
    return p ? fopen(p, mode) : NULL;
}

int fs1a_render(Pc *pc, float *out, int max_samples, int rate)
{
    static FILE *ev;
    static int tried;
    if (!tried) {
        tried = 1;
        ev = open_env("FS1_EVENTS_OUT", "w");
    }
    for (int i = 0; ev && i < pc->speaker_count; i++)
        fprintf(ev, "%llu %02X %04X\n", (unsigned long long)pc->speaker[i].cycle, pc->speaker[i].port61,
                pc->speaker[i].pit2_reload);
    return pc_speaker_render(pc, out, max_samples, rate);
}

SDL_AudioStream *fs1a_open(SDL_AudioDeviceID d, const SDL_AudioSpec *s, SDL_AudioStreamCallback cb, void *u)
{
    (void)d; (void)s; (void)cb; (void)u;
    return (SDL_AudioStream *)(uintptr_t)1;
}
bool fs1a_resume(SDL_AudioStream *s) { (void)s; return true; }
int fs1a_queued(SDL_AudioStream *s) { (void)s; return 0; }
void fs1a_destroy(SDL_AudioStream *s) { (void)s; }
bool fs1a_put(SDL_AudioStream *s, const void *buf, int len)
{
    static FILE *f;
    static int tried;
    (void)s;
    if (!tried) {
        tried = 1;
        f = open_env("FS1_AUDIO_OUT", "wb");
    }
    if (f) {
        fwrite(buf, 1, (size_t)len, f);
        fflush(f);
    }
    return true;
}
"""

RENAMES = {
    "SDL_OpenAudioDeviceStream": "fs1a_open",
    "SDL_ResumeAudioStreamDevice": "fs1a_resume",
    "SDL_GetAudioStreamQueued": "fs1a_queued",
    "SDL_PutAudioStreamData": "fs1a_put",
    "SDL_DestroyAudioStream": "fs1a_destroy",
    "pc_speaker_render": "fs1a_render",
}


def build():
    gcc = shutil.which("gcc") or r"C:\msys64\mingw64\bin\gcc.exe"
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")
    pkg = shutil.which("pkg-config", path=env["PATH"]) or "pkg-config"
    flags = subprocess.run([pkg, "--cflags", "--libs", "sdl3"], capture_output=True, text=True, check=True,
                           env=env).stdout.split()
    src = os.path.join(ROOT, "src")
    rest = [os.path.join(src, f) for f in ("disk.c", "cpu8086.c", "cpu_core.c", "pc.c", "sched.c", "native.c")]
    rest += [os.path.join(src, "natives", f) for f in sorted(os.listdir(os.path.join(src, "natives")))
             if f.endswith(".c")]
    inc = ["-DFS1_EMULATOR=1", "-I", src] + [f for f in flags if f.startswith("-I")]
    with tempfile.TemporaryDirectory() as tmp:
        shim = os.path.join(tmp, "shim.c")
        open(shim, "w").write(SHIM)
        objs = [os.path.join(tmp, "shim.o"), os.path.join(tmp, "main.o")]
        subprocess.run([gcc, "-O2", "-std=c11", "-c", shim, "-o", objs[0]] + inc, check=True, env=env)
        subprocess.run([gcc, "-O2", "-std=c11", "-c", os.path.join(src, "main.c"), "-o", objs[1]] + inc +
                       [f"-D{a}={b}" for a, b in RENAMES.items()], check=True, env=env)
        subprocess.run([gcc, "-O2", "-std=c11"] + inc + rest + objs + ["-o", EXE] + flags, check=True, env=env)


def run(session, off, out_dir):
    name, args, frames = session
    base = os.path.join(out_dir, f"{name}_{'off' if off else 'on'}")
    cmd = [EXE, "--frames", str(frames), "--screenshot", base + ".bmp"]
    for n in off:
        cmd += [n] if n.startswith("--") else ["--native-off", n]
    env = dict(HEADLESS_ENV)
    env["FS1_AUDIO_OUT"] = base + ".f32"
    env["FS1_EVENTS_OUT"] = base + ".events"
    subprocess.run(cmd + args, cwd=ROOT, env=env, capture_output=True, timeout=3600)
    return base


def load(path):
    a = array.array("f")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    return a


def first_diff(a, b):
    k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), None)
    if k is None and len(a) != len(b):
        k = min(len(a), len(b))
    return k


def render_events(events, rate=44100):
    """pc_speaker_render's model on one global sample grid from cycle 0 (port 61h = 0, channel 2
    reload 0533h at boot), so the result does not depend on where the frames ended."""
    cps = 4772727.0 / rate
    ev = [(int(c), int(p, 16), int(r, 16)) for c, p, r in (line.split() for line in events)]
    if not ev:
        return array.array("f")
    n = int((ev[-1][0] + 4772727) / cps)
    out = array.array("f", bytes(4 * n))
    p61, reload, e, phase, x1, y1 = 0, 0x533, 0, 0.0, 0.0, 0.0
    for i in range(n):
        t = int((i + 0.5) * cps)
        while e < len(ev) and ev[e][0] <= t:
            _, p61, reload = ev[e]
            e += 1
        if p61 & 3 == 3:
            freq = 1193182.0 / (reload or 65536)
            if freq > rate / 2:
                x = 0.0
            else:
                phase = (phase + freq / rate) % 1.0
                x = 1.0 if phase < 0.5 else -1.0
        else:
            x = 1.0 if p61 & 2 else -1.0
        y = x - x1 + 0.995 * y1
        x1, y1 = x, y
        out[i] = y * 0.2
    return out


def write_wav(path, samples):
    pcm = array.array("h", (max(-32767, min(32767, int(s * 16000))) for s in samples))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="?", default=",".join(SOUND_NATIVES))
    ap.add_argument("--sessions", default="demo,flight_keys,war")
    ap.add_argument("--wav")
    ap.add_argument("--keep")
    ap.add_argument("--csched", action="store_true")
    o = ap.parse_args()
    off = ["--csched"] if o.csched else o.names.split(",")
    want = o.sessions.split(",")
    chosen = [s for s in sessions() if s[0] in want]
    build()
    failed = False
    tmp_obj = None if o.keep else tempfile.TemporaryDirectory()
    out_dir = o.keep or tmp_obj.name
    os.makedirs(out_dir, exist_ok=True)
    jobs = [(s, f) for s in chosen for f in ([], off)]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        res = list(pool.map(lambda j: run(j[0], j[1], out_dir), jobs))
    for i, s in enumerate(chosen):
        on, of = res[2 * i], res[2 * i + 1]
        a, b = load(on + ".f32"), load(of + ".f32")
        ea, eb = open(on + ".events").read().splitlines(), open(of + ".events").read().splitlines()
        same_shot = open(on + ".bmp", "rb").read() == open(of + ".bmp", "rb").read()
        d, ed = first_diff(a, b), first_diff(ea, eb)
        ra, rb = render_events(ea), render_events(eb)
        rd = first_diff(ra, rb)
        rms = math.sqrt(sum(x * x for x in ra) / len(ra)) if ra else 0.0
        ok = rd is None and ed is None and same_shot and len(ra) > 0
        failed |= not ok
        print(f"{s[0]:14s} speaker events {len(ea):6d} "
              + ("identical" if ed is None else f"differ at event {ed}: on {ea[ed:ed + 3]} off {eb[ed:ed + 3]}")
              + f", screenshot {'identical' if same_shot else 'differs'}")
        print(f"{'':14s} audio from the events: {len(ra)} samples ({len(ra) / 44100:.1f} s), rms {rms:.4f}, "
              + ("identical" if rd is None else f"differs at sample {rd}"))
        print(f"{'':14s} audio as played (per-frame slices): " + ("identical" if d is None else
              f"differs from sample {d} (a frame ends at another cycle when a native is one CPU step)"))
        if o.csched:
            n = min(len(ea), len(eb))
            ca = [int(x.split()[0]) for x in ea[:n]]
            cb = [int(x.split()[0]) for x in eb[:n]]
            same_state = sum(1 for i in range(n) if ea[i].split()[1:] == eb[i].split()[1:])
            dev = sorted(abs(x - y) for x, y in zip(ca, cb))
            print(f"{'':14s} emulated vs C scheduler: {len(ea)} / {len(eb)} events, {same_state} of the first {n} "
                  f"with the same port 61h / channel 2 state, edge time difference median "
                  f"{dev[len(dev) // 2] if dev else 0} cycles, 99th percentile {dev[len(dev) * 99 // 100] if dev else 0}")
        if o.wav:
            os.makedirs(o.wav, exist_ok=True)
            write_wav(os.path.join(o.wav, f"{s[0]}_on.wav"), ra)
            write_wav(os.path.join(o.wav, f"{s[0]}_off.wav"), rb)
    if tmp_obj:
        tmp_obj.cleanup()
    print("FAIL" if failed else "OK")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
