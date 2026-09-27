#!/usr/bin/env bash
# Optimization metrics for one build (Release by default):
#   gameplay scenario unlocked frame time (t50-90 of scenario A),
#   second scenario unlocked frame time + hitches,
#   CPU / RAM / GPU power at 60 fps, VRAM (dev build info log).
# usage: SCEN_A=bench/scenario_a.txt SCEN_B=bench/scenario_b.txt bench/opt_baseline.sh <tag> [extra cvars]
# REL=<exe dir> selects another build. Results: artifacts/profiles/opt_<tag>.txt
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 1
set -a; source kit.env; set +a
TAG=$1; shift
REL=${REL:-$ROOT/$PORT_DIR/out/build/$BUILD_REL}
A=${SCEN_A:?set SCEN_A}; B=${SCEN_B:-$A}
out=artifacts/profiles/opt_$TAG.txt
: > "$out"
EXE_DIR=$REL bash bench/run_safe.sh 200 "opt_${TAG}_a" "$A" 92 --unlocked_vblank_rate=1000 "$@" > /dev/null 2>&1
echo "A unlocked: $(python tools/bench_summary.py "artifacts/profiles/opt_${TAG}_a.csv" 40 50 90 | tail -1 | cut -c10-60)" | tee -a "$out"
EXE_DIR=$REL bash bench/run_safe.sh 260 "opt_${TAG}_b" "$B" 130 --unlocked_vblank_rate=1000 "$@" > /dev/null 2>&1
python - "$TAG" <<'PY' | tee -a "$out"
import csv, sys
rows = list(csv.DictReader(open(f'artifacts/profiles/opt_{sys.argv[1]}_b.csv')))
ft = sorted(int(r['frame_time_us']) / 1000 for r in rows if float(r['t_ms']) > 40000)
print(f'B unlocked (t>40s): mean {sum(ft)/len(ft):.2f} ms p99 {ft[int(len(ft)*0.99)]:.2f} '
      f'>50ms {sum(f > 50 for f in ft)} >100ms {sum(f > 100 for f in ft)}')
PY
(python tools/power_probe.py 15 50 "artifacts/profiles/opt_${TAG}_pw.txt" > /dev/null &)
(powershell -NoProfile -File tools/mem_probe.ps1 60 > "artifacts/profiles/opt_${TAG}_mem.txt" &)
EXE_DIR=$REL bash bench/run_safe.sh 200 "opt_${TAG}_60" "$A" 70 --fps_limit=60 "$@" > /dev/null 2>&1
sleep 3
echo "60 fps: $(cat "artifacts/profiles/opt_${TAG}_pw.txt") $(tr -d '\r\n' < "artifacts/profiles/opt_${TAG}_mem.txt")" | tee -a "$out"
bash bench/run_safe.sh 200 "opt_${TAG}_vr" "$A" 70 --fps_limit=60 "$@" > /dev/null 2>&1
LOGS=$PORT_DIR/out/build/$BUILD_DEV/logs
echo "vram: $(grep 'native: vram' "$LOGS/$(ls -t "$LOGS" | head -1)" | tail -1 | cut -c50-240)" | tee -a "$out"
