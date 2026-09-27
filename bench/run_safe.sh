#!/usr/bin/env bash
# bench/run.sh with a wall-clock limit: kills the run's game process if it hangs.
# usage: bench/run_safe.sh <limit_s> <run.sh args...>
# Only processes started from this run's EXE_DIR are killed, never a copy of the
# game the user has open elsewhere (release/, another build). NEVER kill by image
# name: the user may be playing the release at the same time.
LIMIT=$1; shift
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
set -a; source "$ROOT/kit.env"; set +a
EXE_DIR="${EXE_DIR:-$ROOT/$PORT_DIR/out/build/$BUILD_DEV}"
export EXE_DIR
timeout "$LIMIT" "$ROOT/bench/run.sh" "$@"
rc=$?
if [ $rc -eq 124 ]; then
  echo "[run_safe] run exceeded ${LIMIT}s, killing $GAME_NAME.exe from $EXE_DIR" >&2
  DIR_WIN="$(cygpath -w "$EXE_DIR")"
  powershell -NoProfile -Command "Get-Process $GAME_NAME -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$DIR_WIN', [System.StringComparison]::OrdinalIgnoreCase) } | Stop-Process -Force" > /dev/null 2>&1
fi
exit $rc
