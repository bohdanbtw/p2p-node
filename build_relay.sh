#!/usr/bin/env bash
# Headless relay/bootstrap build.  No SDL2, ImGui or Opus is linked.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="${SYSROOT:-/tmp/opencode/sysroot}"
CXX="${CXX:-g++}"

CXXFLAGS=(
  -std=c++17 -O2 -g -Wall -Wextra -Wno-unused-parameter
  -I"$ROOT/src"
  -I"$SYSROOT/usr/include"
  -I"$SYSROOT/usr/include/x86_64-linux-gnu"
)

"$CXX" "${CXXFLAGS[@]}" \
  "$ROOT/src/relay_main.cpp" "$ROOT/src/net.cpp" "$ROOT/src/crypto.cpp" \
  "$ROOT/src/dht.cpp" \
  -L"$SYSROOT/lib" -lsodium -lpthread -ldl -lrt -lm \
  -o "$ROOT/build/p2p-relay"

echo "built: $ROOT/build/p2p-relay"
