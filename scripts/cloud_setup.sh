#!/usr/bin/env bash
# Prepares a headless Linux session (cloud agent, CI): tools, Python deps, the original
# disk image from the repo's private "disk-image" release, a build, and the analysis files.
# Works on Debian/Ubuntu and on Windows Git Bash with MSYS2 (C:/msys64).
# Usage: bash scripts/cloud_setup.sh
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE="original/Microsoft Flight Simulator v1.05 (198x)(Microsoft Corporation).ima"
PY=python3

case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    # Windows (Git Bash): use MSYS2's MinGW toolchain and SDL3; no apt. Keep the regular
    # Windows Python (picked before PATH changes), not MSYS2's, which has no pip.
    PY="$(command -v python)"
    export PATH="/c/msys64/mingw64/bin:$PATH"
    WINDOWS=1
    ;;
esac

if [ -z "${WINDOWS:-}" ] && command -v apt-get >/dev/null; then
    SUDO=$(command -v sudo || true)
    $SUDO apt-get update -qq
    $SUDO apt-get install -y -qq build-essential cmake ninja-build git python3 python3-pip curl >/dev/null
fi
$PY -m pip install -q -r requirements.txt || $PY -m pip install -q --break-system-packages -r requirements.txt

if [ ! -f "$IMAGE" ]; then
    mkdir -p original
    if command -v gh >/dev/null && gh auth status >/dev/null 2>&1; then
        gh release download disk-image --pattern "*.ima" --output "$IMAGE"
    elif [ -n "${GITHUB_TOKEN:-}" ]; then
        repo=$(git remote get-url origin | sed -E 's#.*github.com[:/]##; s#\.git$##')
        id=$(curl -fsSL -H "Authorization: Bearer $GITHUB_TOKEN" \
             "https://api.github.com/repos/$repo/releases/tags/disk-image" |
             $PY -c "import json,sys; print(json.load(sys.stdin)['assets'][0]['id'])")
        curl -fsSL -H "Authorization: Bearer $GITHUB_TOKEN" -H "Accept: application/octet-stream" \
             -o "$IMAGE" "https://api.github.com/repos/$repo/releases/assets/$id"
    else
        echo "No disk image and no GitHub access (gh or GITHUB_TOKEN) to fetch it." >&2
        exit 1
    fi
fi
size=$(wc -c < "$IMAGE" | tr -d ' ')
[ "$size" = 163840 ] || { echo "Unexpected image size $size" >&2; exit 1; }

# build/: the game (C scheduler, no interpreter). build-emu/: FS1_EMULATOR=ON, the 8086 interpreter and
# emulated PC that the verification tools (trace_campaign, verify_campaign, insn_stats ...) run.
for cfg in "build OFF" "build-emu ON"; do
    set -- $cfg
    if [ -n "${WINDOWS:-}" ]; then
        cmake -S . -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_PREFIX_PATH=C:/msys64/mingw64 -DFS1_EMULATOR=$2
    else
        cmake -S . -B "$1" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFS1_EMULATOR=$2
    fi
    cmake --build "$1"
done
$PY tools/pc_loadstream.py "$IMAGE" --dump extracted
# A local worktree can reuse the main checkout's trace instead of a 15-minute campaign.
main_trace="$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)/../extracted/trace.bin"
if [ ! -f extracted/trace.bin ] && [ -f "$main_trace" ]; then
    cp "$main_trace" extracted/trace.bin
fi
[ -f extracted/trace.bin ] || $PY tools/trace_campaign.py --fresh
$PY tools/fs1dis.py
echo "Setup complete."
