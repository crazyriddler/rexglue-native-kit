#!/usr/bin/env bash
# Captures the base pipeline (PSO) list embedded in the exe (RCDATA 3, see
# docs/NATIVE_RENDERER_ARCHITECTURE.md "pipeline cache"): runs the given bench
# scenarios with an empty <GAME_NAME>_pipelines.bin, then MERGES the recorded
# pipelines into artifacts/shaders/pipelines_base.bin (never replaces it: a
# replace once dropped 175 -> 31 records). Rebuild afterwards (re-run cmake so
# the RC picks the file up) to embed it.
# usage: tools/make_pipeline_base.sh name:secs [name:secs ...]
#   e.g. tools/make_pipeline_base.sh fmv:60 newgame:130 level1:95 pause:95
#   (runs bench/scenario_<name>.txt for <secs> seconds each)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
set -a; source "$ROOT/kit.env"; set +a
EXE_DIR="$ROOT/$PORT_DIR/out/build/$BUILD_DEV"
PL="$EXE_DIR/${GAME_NAME}_pipelines.bin"
cd "$ROOT"
[ $# -gt 0 ] || { echo "usage: $0 name:secs [...]" >&2; exit 1; }
rm -f "$PL"
for s in "$@"; do
  name=${s%%:*}; secs=${s##*:}
  echo "[pipelines] scenario $name"
  bash bench/run_safe.sh $((secs + 120)) "plbase_$name" "bench/scenario_$name.txt" "$secs" \
    --fps_limit=60 > /dev/null 2>&1 || true
done
mkdir -p artifacts/shaders
# Merge: the local file only holds pipelines the embedded base did not know.
# Paths through cygpath: Windows Python does not see /tmp-style paths.
python - "$(cygpath -w "$PL")" "$(cygpath -w "$ROOT/artifacts/shaders/pipelines_base.bin")" <<'PYEOF'
import os, struct, sys
local, base = sys.argv[1], sys.argv[2]
def load(p):
    if not os.path.exists(p):
        return None, []
    d = open(p, 'rb').read()
    rs = struct.unpack('<III', d[4:16])[1]
    return d[:16], [d[16 + i * rs:16 + (i + 1) * rs] for i in range((len(d) - 16) // rs)]
hb, a = load(base)
if not os.path.exists(local):
    # The native renderer creates this file at every start: missing = the renderer did not
    # run or it writes another name (pipeline_cache.cpp must use <GAME_NAME>_pipelines.bin).
    sys.exit(f'[pipelines] ERROR: {local} was not created by the runs; base left unchanged')
hl, b = load(local)
header = hl or hb
if header is None:
    sys.exit('[pipelines] nothing recorded')
if hb and hl and hb != hl:
    a = []  # different record format/version: start over from the local file
seen, out = set(), []
for r in a + b:
    if r[:8] not in seen:
        seen.add(r[:8])
        out.append(r)
open(base, 'wb').write(header + b''.join(out))
print(f'[pipelines] {len(a)} base + {len(b)} new -> {len(out)} -> {base}')
PYEOF
