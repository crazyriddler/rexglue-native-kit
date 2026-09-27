#!/usr/bin/env bash
# usage: SCEN=bench/scenario_x.txt bench/ab_swap.sh <name> <swap> <exit_s> [extra]
# Frame-exact A/B of one guest swap (native full-frame dump vs Xenos screenshot).
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 1
set -a; source kit.env; set +a
N=$1; S=$2; E=$3; shift 3
D=artifacts/captures/$N; mkdir -p "$D"; rm -f "${D:?}"/*
rm -rf "artifacts/screenshots/${N:?}"
bench/run_safe.sh 200 "$N" "${SCEN:?set SCEN=bench/scenario_x.txt}" "$E" --native_renderer=true \
  --native_ab_mode=true --native_dump_swap="$S" --native_dump_dir="../../../../$D" \
  --bench_screenshot_swaps="$S" "$@"
python tools/native_dump_to_png.py "$D" > "$D/summary.txt"
OUT="$D/output_${GUEST_WIDTH}x${GUEST_HEIGHT}.png"
X=$(printf "artifacts/screenshots/$N/swap_%06d.bmp" "$S")
python tools/ab_diff.py "$OUT" "$X" "artifacts/captures/${N}_diff.png"
