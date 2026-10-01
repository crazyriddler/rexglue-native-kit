#!/usr/bin/env bash
# Builds the Release configuration and assembles a clean, portable game folder:
#   $GAME_NAME.exe        game (native renderer, shaders + pipeline base embedded)
#   rexruntime.dll        ReXGlue runtime
#   <GAME_NAME>_*.dll     recompiled guest modules, if the manifest has [[modules]]
#   msvcp140*.dll, vcruntime140*.dll   Visual C++ runtime (app-local)
#   $GAME_NAME.cfg        settings (defaults = original game), edited by the launcher
#   data/                 game data (mirror of $PORT_DIR/game)
# Saves stay in the user's Documents\<game> folder (SDK default user data root).
#
# usage: tools/make_release.sh [name]   (default: <GAME_NAME>-Native)
# Output: release/<name>/ at the kit root. An existing .cfg there is kept (it may
# be the user's own). Nothing else is written into the folder.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KIT_ENV_QUIET=1 source "$ROOT/scripts/dev_env.sh"
NAME="${1:-${GAME_NAME}-Native}"
OUT="$ROOT/release/$NAME"
BUILD="$ROOT/$PORT_DIR/out/build/$BUILD_REL"
GAME="$ROOT/$PORT_DIR/game"
if [ -d "$VC_REDIST_GLOB" ]; then CRT_DIR="$VC_REDIST_GLOB"   # kit-local (setup_toolchain.sh)
else CRT_DIR="$(ls -d $VC_REDIST_GLOB 2>/dev/null | sort | tail -1)"; fi

bash "$ROOT/bench/build.sh" "$BUILD_REL"

mkdir -p "$OUT"
for f in "$GAME_NAME.exe" rexruntime.dll; do cp -f "$BUILD/$f" "$OUT/$f"; done
# Recompiled guest modules ([[modules]] in the manifest, e.g. level DLLs loaded through
# XexLoadImage) are host DLLs named <GAME_NAME>_<module>.dll next to the exe.
for f in "$BUILD/${GAME_NAME}_"*.dll; do [ -e "$f" ] && cp -f "$f" "$OUT/"; done
if [ -n "$CRT_DIR" ]; then
  for f in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll; do
    cp -f "$CRT_DIR/$f" "$OUT/$f"
  done
else
  echo "[release] WARNING: VC++ redist folder not found ($VC_REDIST_GLOB)" >&2
fi

# Default settings (the app writes them and exits with --write_default_settings).
if [ ! -f "$OUT/$GAME_NAME.cfg" ]; then
  (cd "$OUT" && "./$GAME_NAME.exe" --write_default_settings=true) || true
fi

# Game data: mirror into data/ (incremental; //XJ skips junctions such as a
# self-referencing shaders/Shaders link added for a VFS quirk).
set +e
robocopy "$(cygpath -w "$GAME")" "$(cygpath -w "$OUT/data")" //MIR //XJ //DCOPY:T //NFL //NDL //NJH //NJS //NP > /dev/null
rc=$?
set -e
if [ $rc -ge 8 ]; then echo "[release] robocopy failed ($rc)" >&2; exit 1; fi

echo "[release] $OUT"
ls -la "$OUT"
du -sh "$OUT"
