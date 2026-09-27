#!/usr/bin/env bash
# usage: bench/ab_multi.sh <name> <scenario.txt> <exit_s> <swap,swap,...> [extra cvars]
# Frame-exact A/B at several guest swaps in one run: native output dumps
# (native_ab_swaps) vs Xenos captures (bench_screenshot_swaps). Needs the dev
# build (Xenos plugin available) and native_ab_mode in the renderer.
# Prints PSNR per swap and writes artifacts/captures/<name>_sheet.png
# (native | xenos | diff x8). Compare by guest swap number, never by time.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT" || exit 1
KIT_ENV_QUIET=1 source scripts/dev_env.sh
N=$1; SCEN=$2; E=$3; S=$4; shift 4
D=artifacts/captures/$N; mkdir -p "$D"; rm -f "${D:?}"/*
rm -rf "artifacts/screenshots/${N:?}"
# The dump dir is relative to the exe dir ($PORT_DIR/out/build/<b>): the repo
# path may contain spaces and absolute paths get split by the cvar parser.
bench/run_safe.sh $((E + 120)) "$N" "$SCEN" "$E" --native_renderer=true --native_ab_mode=true \
  --native_ab_swaps="$S" --native_dump_dir="../../../../$D" --bench_screenshot_swaps="$S" "$@"
python tools/native_dump_to_png.py "$D" > /dev/null
python tools/ab_multi.py "$D" "artifacts/screenshots/$N" "$S" "artifacts/captures/${N}_sheet.png"
