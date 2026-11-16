#!/usr/bin/env bash
# Rebuild the MinGW-w64 sysroot used by build_win.sh.
#
# The sysroot lives under /tmp and is therefore lost on reboot or tmp cleanup.
# Without it build_win.sh fails with "SDL.h: No such file or directory", which
# looks like a code problem but is only a missing SDK.  Pin the package
# versions: the newest MSYS2 builds expect a C23 runtime (memset_explicit) that
# the Debian/Ubuntu mingw-w64 13 toolchain does not provide.
set -euo pipefail

PREFIX="${MINGW_PREFIX:-/tmp/mingw-build/mingw64}"
BASE=https://repo.msys2.org/mingw/mingw64
PKGS=(
  mingw-w64-x86_64-SDL2-2.32.2-1-any.pkg.tar.zst
  mingw-w64-x86_64-opus-1.5.2-1-any.pkg.tar.zst
  mingw-w64-x86_64-libsodium-1.0.20-1-any.pkg.tar.zst
)
DL="${DL:-/tmp/opencode/pkgs}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX_ROOT="$(dirname "$PREFIX")"

command -v zstd >/dev/null || { echo "need zstd (apt install zstd)"; exit 1; }
command -v x86_64-w64-mingw32-g++ >/dev/null || {
    echo "need mingw-w64 (apt install g++-mingw-w64-x86-64)"; exit 1; }

mkdir -p "$PREFIX_ROOT" "$DL"
cd "$DL"
for f in "${PKGS[@]}"; do
    [ -f "$f" ] || curl -sSLO "$BASE/$f"
    tar --zstd -xf "$f" -C "$PREFIX_ROOT"
done

# Prefer the static archives so the produced .exe needs no SDL2/opus/sodium
# DLLs next to it.  ld picks libfoo.dll.a over libfoo.a, so move them aside.
cd "$PREFIX/lib"
for n in SDL2 opus sodium; do
    [ -f "lib$n.dll.a" ] && mv "lib$n.dll.a" "lib$n.dll.a.off"
done

test -f "$PREFIX/include/SDL2/SDL.h"
test -f "$PREFIX/include/opus/opus.h"
test -f "$PREFIX/include/sodium.h"
echo "sysroot ready: $PREFIX"
