#!/usr/bin/env bash
# usage: bench/build.sh [build dir name, default $BUILD_DEV]
# Builds the game + SDK and ALWAYS syncs the SDK DLLs into the exe dir: CMake
# only copies them when the exe relinks, so an SDK-only change would otherwise
# run against a stale rexruntime.dll (a trap that invalidated a test once).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KIT_ENV_QUIET=1 source "$ROOT/scripts/dev_env.sh"
B=${1:-$BUILD_DEV}
cd "$ROOT/$PORT_DIR"
cmake --build "out/build/$B" -- -j"$(nproc)" 2>&1 | grep -E "error:|FAILED" && exit 1
SFX=""; grep -q "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo" "out/build/$B/CMakeCache.txt" && SFX=rd
SDK=$(grep "^REXSDK_DIR:PATH=" "out/build/$B/CMakeCache.txt" | cut -d= -f2)
for d in rexruntime rexgpu-xenos; do
  [ -f "$SDK/out/win-amd64/$d$SFX.dll" ] && cp -f "$SDK/out/win-amd64/$d$SFX.dll" "out/build/$B/"
done
echo "built+synced $B"
