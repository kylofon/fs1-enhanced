#!/usr/bin/env python3
"""Build the Windows release zip (S4.1), like the StreetRod / Test Drive ports.

    python tools/package.py [--skip-build]

Builds the default (no emulator) Release build in build/, then writes to release/ (gitignored):
  fs1enhanced-vX.Y.Z-win64/      fs1-enhanced.exe and the launcher "FS1 Enhanced.exe" (stripped), the MSYS2 DLLs
                             they load (SDL3, wxWidgets and their dependencies), README.md,
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
REPO_URL = "https://github.com/kylofon/fs1-enhanced"

# Licence files shipped per MSYS2 package (source under mingw64/share/licenses/<dir>).
LICENSES = {
    "mingw-w64-x86_64-sdl3": [("SDL3/LICENSE.txt", "SDL3-LICENSE.txt")],
    "mingw-w64-x86_64-libiconv": [("libiconv/COPYING.LIB", "libiconv-COPYING.LIB.txt")],
    "mingw-w64-x86_64-wxwidgets3.2-common-libs": [("wxWidgets3.2/licence.txt", "wxWidgets-licence.txt"),
                                                  ("wxWidgets3.2/lgpl.txt", "wxWidgets-lgpl.txt")],
    "mingw-w64-x86_64-wxwidgets3.2-msw-libs": [],
    "mingw-w64-x86_64-gcc-libs": [("gcc-libs/COPYING.RUNTIME", "gcc-COPYING.RUNTIME.txt"),
                                  ("gcc-libs/COPYING3", "gcc-COPYING3.txt")],
    "mingw-w64-x86_64-libwinpthread": [("libwinpthread/COPYING", "winpthreads-COPYING.txt")],
    "mingw-w64-x86_64-libjpeg-turbo": [("libjpeg-turbo/LICENSE.md", "libjpeg-turbo-LICENSE.md")],
    "mingw-w64-x86_64-libpng": [("libpng/LICENSE", "libpng-LICENSE.txt")],
    "mingw-w64-x86_64-libtiff": [("libtiff/LICENSE.md", "libtiff-LICENSE.md")],
    "mingw-w64-x86_64-libdeflate": [("libdeflate/COPYING", "libdeflate-COPYING.txt")],
    "mingw-w64-x86_64-jbigkit": [("jbigkit/COPYING", "jbigkit-COPYING.txt")],
    "mingw-w64-x86_64-lerc": [("lerc/LICENSE", "lerc-LICENSE.txt")],
    "mingw-w64-x86_64-xz": [("xz/COPYING", "xz-COPYING.txt"), ("xz/COPYING.0BSD", "xz-COPYING.0BSD.txt")],
    "mingw-w64-x86_64-libwebp": [("libwebp/COPYING", "libwebp-COPYING.txt")],
    "mingw-w64-x86_64-zlib": [("zlib/LICENSE", "zlib-LICENSE.txt")],
    "mingw-w64-x86_64-zstd": [("zstd/LICENSE", "zstd-LICENSE.txt")],
    "mingw-w64-x86_64-pcre2": [("pcre2/LICENCE.md", "pcre2-LICENCE.md")],
}
NOTES = {
    "mingw-w64-x86_64-sdl3": "SDL 3, zlib license, see SDL3-LICENSE.txt. https://libsdl.org",
    "mingw-w64-x86_64-libiconv": "GNU libiconv, needed by SDL3.dll\n"
                                 "GNU LGPL 2.1 or later, see libiconv-COPYING.LIB.txt.\n"
                                 "Source: https://ftp.gnu.org/pub/gnu/libiconv/",
    "mingw-w64-x86_64-wxwidgets3.2-common-libs": "wxWidgets 3.2, needed by the launcher\n"
                                                 "wxWindows Library Licence 3.1, see wxWidgets-licence.txt. https://wxwidgets.org",
    "mingw-w64-x86_64-wxwidgets3.2-msw-libs": "wxWidgets 3.2, see above",
    "mingw-w64-x86_64-gcc-libs": "GCC runtime libraries, GPL 3 with the GCC Runtime Library Exception,\n"
                                 "see gcc-COPYING.RUNTIME.txt. https://gcc.gnu.org",
    "mingw-w64-x86_64-libwinpthread": "mingw-w64 winpthreads, MIT/BSD style, see winpthreads-COPYING.txt",
    "mingw-w64-x86_64-libjpeg-turbo": "libjpeg-turbo, IJG / BSD licences, see libjpeg-turbo-LICENSE.md",
    "mingw-w64-x86_64-libpng": "libpng, PNG Reference Library License, see libpng-LICENSE.txt",
    "mingw-w64-x86_64-libtiff": "libtiff, BSD style, see libtiff-LICENSE.md",
    "mingw-w64-x86_64-libdeflate": "libdeflate, MIT, see libdeflate-COPYING.txt",
    "mingw-w64-x86_64-jbigkit": "JBIG-KIT, GNU GPL 2 or later, see jbigkit-COPYING.txt.\n"
                                "Source: https://www.cl.cam.ac.uk/~mgk25/jbigkit/",
    "mingw-w64-x86_64-lerc": "LERC, Apache 2.0, see lerc-LICENSE.txt",
    "mingw-w64-x86_64-xz": "XZ Utils liblzma, 0BSD, see xz-COPYING.txt",
    "mingw-w64-x86_64-libwebp": "libwebp, BSD, see libwebp-COPYING.txt",
    "mingw-w64-x86_64-zlib": "zlib, zlib licence, see zlib-LICENSE.txt",
    "mingw-w64-x86_64-zstd": "Zstandard, BSD, see zstd-LICENSE.txt",
    "mingw-w64-x86_64-pcre2": "PCRE2, BSD, see pcre2-LICENCE.md",
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
    launcher = ROOT / "build" / "FS1 Enhanced.exe"

    name = f"fs1enhanced-v{ver}-win64"
    rel = ROOT / "release"
    out = rel / name
    shutil.rmtree(out, ignore_errors=True)
    (out / "licenses").mkdir(parents=True)

    for e in (exe, launcher):
        shutil.copy2(e, out / e.name)
        run([str(MINGW / "bin" / "strip.exe"), str(out / e.name)])
    for f in ("README.md", "CHANGELOG.md", "LICENSE"):
        shutil.copy2(ROOT / f, out / f)

    dlls = mingw_dlls(exe) + [d for d in mingw_dlls(launcher) if d not in mingw_dlls(exe)]
    third = ["Third-party components shipped with fs1enhanced", "",
             "fs1-enhanced.exe and FS1 Enhanced.exe are MIT licensed, see ../LICENSE.", f"Its source: {REPO_URL}", "",
             "All DLLs below are unmodified builds from MSYS2 (https://packages.msys2.org, mingw64",
             "repository); their sources are available from the MSYS2 package pages and the upstream",
             "projects named here. You may replace any of them with your own build.", ""]
    for dll in dlls:
        shutil.copy2(MINGW / "bin" / dll, out / dll)
        pkg, pver = owning_package(dll)
        for src, dst in LICENSES.get(pkg, []):
            if (out / "licenses" / dst).exists():
                continue
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

    notes = (f"FS1 Enhanced v{ver}: enhanced SDL3 port of Microsoft Flight Simulator 1.05 (Windows x64).\n\n"
             f"Unzip `{zpath.name}` into a folder, start `FS1 Enhanced.exe` (the launcher) and pick your original PC\n"
             "disk image (`.ima`, 160K), or put the image next to `fs1-enhanced.exe` and start that. The original game\n"
             "is not included. See the README for the keys and options.\n\n"
             + changelog_section(ver) + "\n")
    (rel / "RELEASE_NOTES.md").write_text(notes, encoding="utf-8")
    digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
    (rel / "SHA256SUMS.txt").write_text(f"{digest} *{zpath.name}\n")

    files = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    print(f"{zpath.relative_to(ROOT)}: {zpath.stat().st_size} bytes, {len(files)} files: {', '.join(files)}")
    print(f"sha256 {digest}")


if __name__ == "__main__":
    main()
