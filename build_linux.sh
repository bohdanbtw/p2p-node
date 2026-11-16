#!/usr/bin/env bash
# Direct Linux build (no CMake required). Headers/libs are taken from
# /tmp/opencode/sysroot in this environment; override SYSROOT on another machine.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
# Empty SYSROOT means system headers (/usr); default kept for this environment.
SYSROOT="${SYSROOT-/tmp/opencode/sysroot}"
CXX="${CXX:-g++}"
# jech/dht is C89-style C (uses `xor`, a C++ keyword), so it is compiled with
# the C compiler and linked with the C++ objects.
CC="${CC:-gcc}"

CXXFLAGS=(
  -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
  -I"$ROOT/src"
  -I"$ROOT/third_party/imgui"
  -I"$ROOT/third_party/imgui/backends"
  -I"$SYSROOT/usr/include"
  -I"$SYSROOT/usr/include/SDL2"
  -I"$SYSROOT/usr/include/x86_64-linux-gnu"
)

SOURCES=(
  "$ROOT/src/main.cpp"
  "$ROOT/src/app.cpp"
  "$ROOT/src/net.cpp"
  "$ROOT/src/voice.cpp"
  "$ROOT/src/crypto.cpp"
  "$ROOT/src/dht.cpp"
  "$ROOT/src/mainline_dht.cpp"
  "$ROOT/third_party/imgui/imgui.cpp"
  "$ROOT/third_party/imgui/imgui_draw.cpp"
  "$ROOT/third_party/imgui/imgui_tables.cpp"
  "$ROOT/third_party/imgui/imgui_widgets.cpp"
  "$ROOT/third_party/imgui/backends/imgui_impl_sdl2.cpp"
  "$ROOT/third_party/imgui/backends/imgui_impl_opengl3.cpp"
)

mkdir -p "$ROOT/build"
# The Mainline DHT library (jech/dht) is plain C: build it separately, link the object.
"$CC" -O2 -Wall -c "$ROOT/third_party/dht/dht.c" -o "$ROOT/build/dht.o"

"$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" "$ROOT/build/dht.o" \
  -L"$SYSROOT/lib" -lSDL2 -lopus -lsodium -lGL -lX11 -lpthread -ldl -lrt -lm \
  -o "$ROOT/build/p2pnode"

echo "built: $ROOT/build/p2pnode"
