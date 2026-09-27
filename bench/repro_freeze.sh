#!/usr/bin/env bash
# usage: bench/repro_freeze.sh <runs> <scenario.txt> [cvars]
# Runs a scenario N times (dev build, logs on) and classifies each run:
# HANG = the hang watchdog fired (no guest swap for native_hang_watchdog_s; it
# logs every thread's stack). Use it to turn "sometimes freezes" into a rate.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 1
KIT_ENV_QUIET=1 source scripts/dev_env.sh
N=$1; SCEN=$2; shift 2
LOGS=$PORT_DIR/out/build/$BUILD_DEV/logs
hangs=0
for i in $(seq 1 "$N"); do
  bench/run_safe.sh 200 "repro_$i" "$SCEN" 100 "$@" > /dev/null 2>&1
  L=$LOGS/$(ls -t "$LOGS" | head -1)
  h=$(grep -c "native watchdog: no swap" "$L"); d=$(grep -c "dropping frame" "$L")
  last=$(grep -o "swap [0-9]*" "$L" | tail -1)
  if [ "$h" != "0" ]; then st=HANG; hangs=$((hangs+1)); else st=clean; fi
  echo "run $i: $st drops=$d $(basename "$L") last=$last"
done
echo "hangs $hangs / $N"
