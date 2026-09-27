#!/usr/bin/env bash
# usage: bench/run.sh <name> <script.txt> <exit_after_s> [extra args...]
# Runs $GAME_NAME.exe (default: the dev build) with a fresh copy of the benchmark
# user data (bench/userdata_template -> bench/userdata_run), scripted input
# (--autoinput_script), a per-frame perf CSV (artifacts/profiles/<name>.csv) and
# screenshots dir artifacts/screenshots/<name>. Exits after <exit_after_s>.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KIT_ENV_QUIET=1 source "$ROOT/scripts/dev_env.sh"
NAME=$1; SCRIPT=$2; SECS=$3; shift 3
UD="$ROOT/bench/userdata_run"
# USERDATA_TEMPLATE=<dir> uses another user data root (e.g. one per save).
TEMPLATE="${USERDATA_TEMPLATE:-$ROOT/bench/userdata_template}"
rm -rf "$UD"; mkdir -p "$TEMPLATE"; cp -r "$TEMPLATE" "$UD"
EXE_DIR="${EXE_DIR:-$ROOT/$PORT_DIR/out/build/$BUILD_DEV}"
cd "$EXE_DIR"
W=$(cygpath -m "$ROOT")
SHOTS="$ROOT/artifacts/screenshots/$NAME"; mkdir -p "$SHOTS" "$ROOT/artifacts/profiles"
# Reproducible defaults (kit.env BENCH_DEFAULTS), added only when the caller did
# not pass the same flag: a flag given twice makes the parser drop every flag.
DEFAULTS=()
for kv in $BENCH_DEFAULTS; do
  case " $* " in *" --${kv%%=*}="*) ;; *) DEFAULTS+=("--$kv") ;; esac
done
# The repo path may contain spaces: pass paths as mixed (C:/...) and keep dump
# dirs relative to the exe dir (see docs/LESSONS_LEARNED.md, tooling traps).
# --gpu_plugin xenos: the legacy renderer before the native graphics system exists
# (ignored once the app installs its own IGraphicsSystem in OnPreSetup).
"./$GAME_NAME.exe" --bench_screenshot_dir="$(cygpath -m "$SHOTS")" --gpu_plugin xenos --mnk_mode \
  --user_data_root="$(cygpath -m "$UD")" \
  --autoinput_script="$(cygpath -m "$ROOT/$SCRIPT")" \
  "${DEFAULTS[@]}" \
  --perf_log_csv="$W/artifacts/profiles/$NAME.csv" --bench_exit_after_s="$SECS" "$@" > /dev/null 2>&1
