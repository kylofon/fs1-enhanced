#!/usr/bin/env python3
"""Build the Windows release zip (S4.1), like the StreetRod / Test Drive ports.

    python tools/package.py [--skip-build]

Builds the default (no emulator) Release build in build/, then writes to release/ (gitignored):
  fs1enhanced-vX.Y.Z-win64/      fs1-enhanced.exe (stripped), SDL3.dll and the MSYS2 DLLs it loads, README.md,
                             CHANGELOG.md, LICENSE, licenses/ (THIRD-PARTY.txt + each DLL's licence)
  fs1enhanced-vX.Y.Z-win64.zip   that folder's files at the zip root
  RELEASE_NOTES.md           the CHANGELOG section of this version, for the GitHub release
  SHA256SUMS.txt
The version comes from project(... VERSION) in CMakeLists.txt. No game data goes in.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MINGW = Path(os.environ.get("MINGW_PREFIX_WIN", r"C:\msys64\mingw64"))
PACMAN_DB = MINGW.parent / "var" / "lib" / "pacman" / "local"
REPO_URL = "https://github.com/kylofon/fs1-sdl3-port"

# Licence files shipped per MSYS2 package (source under mingw64/share/licenses/<dir>).
LICENSES = {
    "mingw-w64-x86_64-sdl3": [("SDL3/LICENSE.txt", "SDL3-LICENSE.txt")],
    "mingw-w64-x86_64-libiconv": [("libiconv/COPYING.LIB", "libiconv-COPYING.LIB.txt")],
}
NOTES = {
    "mingw-w64-x86_64-sdl3": "SDL 3, zlib license, see SDL3-LICENSE.txt. https://libsdl.org",
    "mingw-w64-x86_64-libiconv": "GNU libiconv, needed by SDL3.dll\n"
                                 "GNU LGPL 2.1 or later, see libiconv-COPYING.LIB.txt.\n"
                                 "Source: https://ftp.gnu.org/pub/gnu/libiconv/",
}


def run(cmd):
    env = dict(os.environ, PATH=str(MINGW / "bin") + os.pathsep + os.environ["PATH"])
    subprocess.run(cmd, cwd=ROOT, env=env, check=True)


def version():
    m = re.search(r"project\(fs1 VERSION (\d+\.\d+\.\d+)", (ROOT / "CMakeLists.txt").read_text())
    return m.group(1)


def dll_imports(path):
    out = subprocess.run([str(MINGW / "bin" / "objdump.exe"), "-p", str(path)],
                         capture_output=True, text=True, check=True).stdout
    return re.findall(r"DLL Name: (\S+)", out)


def mingw_dlls(exe):
    """The DLLs from mingw64/bin that exe loads, directly or through each other."""
    found, todo = [], [exe]
    while todo:
        for name in dll_imports(todo.pop()):
            p = MINGW / "bin" / name
            if p.exists() and name not in found:
                found.append(name)
                todo.append(p)
    return found


def owning_package(dll):
    """(package name, version) of the installed MSYS2 package that holds mingw64/bin/<dll>."""
    needle = "mingw64/bin/" + dll + "\n"
    for d in PACMAN_DB.iterdir():
        files = d / "files"
        if files.exists() and needle in files.read_text(errors="replace"):
            desc = (d / "desc").read_text().split("\n")
            return desc[desc.index("%NAME%") + 1], desc[desc.index("%VERSION%") + 1]
    sys.exit(f"package.py: no MSYS2 package owns {dll}")


def changelog_section(ver):
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    m = re.search(rf"^## \[?{re.escape(ver)}\]?.*?\n(.*?)(?=^## |\Z)", text, re.S | re.M)
    if not m:
        sys.exit(f"package.py: CHANGELOG.md has no '## {ver}' section")
    return m.group(1).strip()


def main():
    ver = version()
    if "--skip-build" not in sys.argv:
        run(["cmake", "-S", ".", "-B", "build", "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DFS1_EMULATOR=OFF", f"-DCMAKE_PREFIX_PATH={MINGW.as_posix()}"])
        run(["cmake", "--build", "build"])
    exe = ROOT / "build" / "fs1-enhanced.exe"

    name = f"fs1enhanced-v{ver}-win64"
    rel = ROOT / "release"
    out = rel / name
    shutil.rmtree(out, ignore_errors=True)
    (out / "licenses").mkdir(parents=True)

    shutil.copy2(exe, out / "fs1-enhanced.exe")
    run([str(MINGW / "bin" / "strip.exe"), str(out / "fs1-enhanced.exe")])
    for f in ("README.md", "CHANGELOG.md", "LICENSE"):
        shutil.copy2(ROOT / f, out / f)

    dlls = mingw_dlls(exe)
    third = ["Third-party components shipped with fs1enhanced", "",
             "fs1-enhanced.exe is MIT licensed, see ../LICENSE.", f"Its source: {REPO_URL}", "",
             "All DLLs below are unmodified builds from MSYS2 (https://packages.msys2.org, mingw64",
             "repository); their sources are available from the MSYS2 package pages and the upstream",
             "projects named here. You may replace any of them with your own build.", ""]
    for dll in dlls:
        shutil.copy2(MINGW / "bin" / dll, out / dll)
        pkg, pver = owning_package(dll)
        for src, dst in LICENSES.get(pkg, []):
            shutil.copy2(MINGW / "share" / "licenses" / src, out / "licenses" / dst)
        note = NOTES.get(pkg, "see the MSYS2 package page for its licence")
        third.append(f"{dll:<16}{pkg} {pver}")
        third += [" " * 16 + line for line in note.split("\n")]
        third.append("")
        if pkg not in LICENSES:
            print(f"warning: no licence file mapped for {pkg} ({dll}); add it to LICENSES")
    (out / "licenses" / "THIRD-PARTY.txt").write_text("\n".join(third), encoding="utf-8")

    zpath = rel / f"{name}.zip"
    zpath.unlink(missing_ok=True)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(out.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(out).as_posix())

    notes = (f"Flight Simulator 1 SDL3 port v{ver} (Windows x64).\n\n"
             f"Unzip `{zpath.name}` into a folder, put your original PC disk image (`.ima`, 160K) next to\n"
             "`fs1-enhanced.exe` and start `fs1-enhanced.exe`. The original game is not included. See the README for the keys.\n\n"
             + changelog_section(ver) + "\n")
    (rel / "RELEASE_NOTES.md").write_text(notes, encoding="utf-8")
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    (rel / "SHA256SUMS.txt").write_text(f"{digest} *{zpath.name}\n")

    files = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    print(f"{zpath.relative_to(ROOT)}: {zpath.stat().st_size} bytes, {len(files)} files: {', '.join(files)}")
    print(f"sha256 {digest}")


if __name__ == "__main__":
    main()
