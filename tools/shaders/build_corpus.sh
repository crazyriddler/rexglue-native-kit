#!/usr/bin/env bash
# One-command shader corpus pipeline (M4). Regenerates everything under
# artifacts/shaders and docs/SHADER_CATALOG.md:
#   1. fetch DXC release into tools/dxc (if missing)
#   2. build XenosRecompCorpus from the vendored, patched tools/xenosrecomp/src
#   3. extract containers from $PORT_DIR/game (byte-granular scan) -> raw/ + manifest.json
#   4. join the Xenos-path runtime shader storage (bench/**/<TITLE_ID>.xsh) -> runtime_join.json
#   5. translate (HLSL), compile DXIL (+lib_6_3 for spec-constant shaders) and SPIR-V, reflect -> catalog.json + docs
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
DXC_VER="v1.9.2607"; DXC_ZIP="dxc_2026_07_29.zip"

python -c "import xxhash" 2>/dev/null || python -m pip install --user xxhash

if [ ! -x "$ROOT/tools/dxc/bin/x64/dxc.exe" ]; then
  echo "[corpus] fetching DXC $DXC_VER"
  mkdir -p "$ROOT/tools/dxc"
  curl -sSL -o "$ROOT/tools/dxc/dxc.zip" \
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/$DXC_VER/$DXC_ZIP"
  (cd "$ROOT/tools/dxc" && unzip -q -o dxc.zip 2>/dev/null || true; rm -f dxc.zip; rm -rf bin/arm64 bin/x86 lib/arm64 lib/x86)
  printf '*\n!.gitignore\n' > "$ROOT/tools/dxc/.gitignore"
fi

[ -f "$ROOT/tools/xenosrecomp/src/XenosRecomp/shader_recompiler.cpp" ] || bash "$ROOT/tools/xenosrecomp/fetch_source.sh"
set +u; source "$ROOT/scripts/dev_env.sh" >/dev/null; set -u
cmake -S "$ROOT/tools/xenosrecomp" -B "$ROOT/tools/xenosrecomp/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ >/dev/null
cmake --build "$ROOT/tools/xenosrecomp/build"

python "$HERE/extract_shaders.py" "$@"
python "$HERE/join_runtime_cache.py"
python "$HERE/build_catalog.py"
echo "[corpus] done: $ROOT/artifacts/shaders/catalog.json, $ROOT/docs/SHADER_CATALOG.md"
