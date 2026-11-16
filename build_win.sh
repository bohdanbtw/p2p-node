#!/usr/bin/env bash
# Cross-compile p2pnode for Windows x86_64 MinGW (full GUI build with SDL2/ImGui).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${MINGW_PREFIX:-/tmp/mingw-build/mingw64}"

CXX="${CXX:-x86_64-w64-mingw32-g++}"
# jech/dht is C89-style C (uses `xor`, a C++ keyword) - compiled with the C
# cross-compiler and linked with the C++ objects.
CC="${CC:-x86_64-w64-mingw32-gcc}"

CXXFLAGS=(
  -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
  -D_WIN32_WINNT=0x0601 -mwindows
  -I"$ROOT/src"
  -I"$ROOT/third_party/imgui"
  -I"$ROOT/third_party/imgui/backends"
  -I"$PREFIX/include"
  -I"$PREFIX/include/SDL2"
)

IMGUI_SOURCES=(
  "$ROOT/third_party/imgui/imgui.cpp"
  "$ROOT/third_party/imgui/imgui_draw.cpp"
  "$ROOT/third_party/imgui/imgui_tables.cpp"
  "$ROOT/third_party/imgui/imgui_widgets.cpp"
  "$ROOT/third_party/imgui/backends/imgui_impl_opengl3.cpp"
  "$ROOT/third_party/imgui/backends/imgui_impl_sdl2.cpp"
)

SOURCES=(
  "$ROOT/src/main.cpp"
  "$ROOT/src/app.cpp"
  "$ROOT/src/net.cpp"
  "$ROOT/src/voice.cpp"
  "$ROOT/src/crypto.cpp"
  "$ROOT/src/dht.cpp"
  "$ROOT/src/mainline_dht.cpp"
)

LIBS=(
  -L"$PREFIX/lib"
  # SDL2main supplies WinMain; force it in so the -mwindows startup finds it.
  -Wl,--whole-archive -lSDL2main -Wl,--no-whole-archive -lSDL2
  -lws2_32 -liphlpapi -lbcrypt -luser32 -lkernel32 -lshell32 -lole32 -lcomdlg32 -loleaut32 -luuid
  -ladvapi32 -lgdi32 -limm32 -lmingw32 -lsetupapi -lcfgmgr32 -lversion
  -lopus -lsodium
  -lpthread -lwinmm -lopengl32
  -static-libstdc++ -static-libgcc
)

mkdir -p "$ROOT/build"
echo "[win] cross-compiling p2pnode.exe (GUI, SDL2/ImGui) ..."
# The Mainline DHT library (jech/dht) is plain C: build it separately, link the object.
"$CC" -O2 -Wall -c "$ROOT/third_party/dht/dht.c" -o "$ROOT/build/dht_win.o"
"$CXX" "${CXXFLAGS[@]}" "${SOURCES[@]}" "${IMGUI_SOURCES[@]}" "$ROOT/build/dht_win.o" "${LIBS[@]}" \
  -o "$ROOT/build/p2pnode.exe"
echo "built: $ROOT/build/p2pnode.exe"
ls -lh "$ROOT/build/p2pnode.exe"
