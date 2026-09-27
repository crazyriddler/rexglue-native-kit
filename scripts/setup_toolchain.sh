#!/usr/bin/env bash
# Install the whole build toolchain inside the kit (tools/toolchain/, gitignored): no admin
# rights, no Visual Studio, nothing outside this folder, no PATH/registry changes.
#
#   bash scripts/setup_toolchain.sh --accept-microsoft-license [--debug-tools]
#
# Installs (pinned versions, SHA-256 checked where the file is versioned):
#   python/        Python 3.14 (python-build-standalone) + scripts/requirements-dev.txt
#   cmake/         CMake 3.31 portable
#   bin/ninja.exe  Ninja
#   llvm/          LLVM/Clang portable release (clang, lld, llvm-symbolizer, llvm-rc)
#   xwin_sysroot/  MSVC CRT + Windows SDK headers/libs, unpacked by xwin      (*)
#   vc_redist/     Visual C++ runtime DLLs for the release folder            (*)
#   --debug-tools: procdump/ (Sysinternals), renderdoc/ (portable)
# (*) Microsoft software: downloading it means accepting Microsoft's license terms
#     (https://go.microsoft.com/fwlink/?LinkId=2086102). The kit README tells the user that
#     starting the kit accepts them; the flag records that acceptance explicitly.
#
# Idempotent: a component already present is kept (delete its folder to reinstall).
# Downloads ~1.1 GB (LLVM ~900 MB); needs ~4 GB free. kit.env points at these folders by
# default and scripts/dev_env.sh puts them first on PATH.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TC="$ROOT/tools/toolchain"
DL="$TC/.downloads"

ACCEPT=0; DEBUG_TOOLS=0
for a in "$@"; do
  case "$a" in
    --accept-microsoft-license) ACCEPT=1 ;;
    --debug-tools) DEBUG_TOOLS=1 ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) ;;
  *) echo "[toolchain] Windows (Git Bash) only; on Linux install clang >= 19, cmake, ninja from the distribution (VALIDATION_GUIDE)." >&2; exit 1 ;;
esac

LLVM_VER=23.1.1
LLVM_SHA=c54ac8146b420fe72e11e6fdd56498d6818011ad23267196b6ab37b5ac9264c3
CMAKE_VER=3.31.12
CMAKE_SHA=0c4baa40f28b3f8225eb3fdf6946c987b4fe901403b4eaf2fbbd9378100aaa0c
NINJA_VER=1.13.1
NINJA_SHA=26a40fa8595694dec2fad4911e62d29e10525d2133c9a4230b66397774ae25bf
XWIN_VER=0.10.0
XWIN_SHA=96e83665ef5c1406aabd0be603a8d08e67e604f36fda630b6807ee948a614e26
PY_VER=3.14.7; PY_TAG=20260924
PY_SHA=c9b509adcbbeb824c869bf29adbb58a269107e85ec3f7f3fa5872317806a3beb
RENDERDOC_VER=1.37

mkdir -p "$TC" "$DL" "$TC/bin"

# fetch <url> <file> [sha256]
fetch() {
  local url=$1 out="$DL/$2" sha=${3:-}
  if [ ! -f "$out" ]; then
    echo "[toolchain] downloading $2"
    curl -fL --retry 3 --retry-delay 5 -o "$out.part" "$url"
    mv "$out.part" "$out"
  fi
  if [ -n "$sha" ] && [ "$(sha256sum "$out" | cut -d' ' -f1)" != "$sha" ]; then
    rm -f "$out"
    echo "[toolchain] ERROR: SHA-256 mismatch for $2 (file deleted; re-run)" >&2
    exit 1
  fi
}

unzip_to() {  # unzip_to <zip> <dir>
  mkdir -p "$2"
  if command -v unzip >/dev/null; then unzip -q -o "$1" -d "$2"
  else "$TC/python/python.exe" -m zipfile -e "$1" "$2"; fi
}

# Python first: the rest of the kit's tools need it.
if [ ! -x "$TC/python/python.exe" ]; then
  f="cpython-$PY_VER+$PY_TAG-x86_64-pc-windows-msvc-install_only.tar.gz"
  fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_TAG/${f//+/%2B}" "$f" "$PY_SHA"
  tar -xzf "$DL/$f" -C "$TC"     # -> $TC/python/
fi
"$TC/python/python.exe" -m pip install -q --disable-pip-version-check -r "$ROOT/scripts/requirements-dev.txt"

if [ ! -x "$TC/cmake/bin/cmake.exe" ]; then
  f="cmake-$CMAKE_VER-windows-x86_64.zip"
  fetch "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VER/$f" "$f" "$CMAKE_SHA"
  rm -rf "$TC/cmake-$CMAKE_VER-windows-x86_64"
  unzip_to "$DL/$f" "$TC"
  mv "$TC/cmake-$CMAKE_VER-windows-x86_64" "$TC/cmake"
fi

if [ ! -x "$TC/bin/ninja.exe" ]; then
  fetch "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VER/ninja-win.zip" "ninja-$NINJA_VER-win.zip" "$NINJA_SHA"
  unzip_to "$DL/ninja-$NINJA_VER-win.zip" "$TC/bin"
fi

if [ ! -x "$TC/llvm/bin/clang++.exe" ]; then
  f="clang+llvm-$LLVM_VER-x86_64-pc-windows-msvc.tar.xz"
  fetch "https://github.com/llvm/llvm-project/releases/download/llvmorg-$LLVM_VER/$f" "$f" "$LLVM_SHA"
  echo "[toolchain] extracting LLVM (a few minutes)"
  rm -rf "$TC/clang+llvm-$LLVM_VER-x86_64-pc-windows-msvc"
  tar -xJf "$DL/$f" -C "$TC"
  mv "$TC/clang+llvm-$LLVM_VER-x86_64-pc-windows-msvc" "$TC/llvm"
fi

need_ms=0
[ -f "$TC/xwin_sysroot/crt/include/vcruntime.h" ] || need_ms=1
[ -f "$TC/vc_redist/vcruntime140.dll" ] || need_ms=1
if [ "$need_ms" = 1 ] && [ "$ACCEPT" != 1 ]; then
  echo "[toolchain] the MSVC CRT, Windows SDK and VC++ runtime are Microsoft software: re-run with" >&2
  echo "            --accept-microsoft-license (terms: https://go.microsoft.com/fwlink/?LinkId=2086102)" >&2
  exit 1
fi

if [ ! -f "$TC/xwin_sysroot/crt/include/vcruntime.h" ]; then
  f="xwin-$XWIN_VER-x86_64-pc-windows-msvc.tar.gz"
  fetch "https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VER/$f" "$f" "$XWIN_SHA"
  tar -xzf "$DL/$f" -C "$DL"
  echo "[toolchain] xwin: downloading and unpacking the MSVC CRT + Windows SDK"
  rm -rf "$TC/xwin_sysroot"
  # --disable-symlinks: the casing symlinks need a privilege Windows users lack and NTFS is
  # case-insensitive anyway. Layout: crt/{include,lib/x86_64}, sdk/{include,lib}/... (dev_env.sh).
  "$DL/xwin-$XWIN_VER-x86_64-pc-windows-msvc/xwin.exe" --accept-license --http-retry 3 \
    --cache-dir "$(cygpath -w "$DL/xwin-cache")" \
    splat --include-debug-libs --disable-symlinks --output "$(cygpath -w "$TC/xwin_sysroot")"
  rm -rf "$DL/xwin-cache"
fi

if [ ! -f "$TC/vc_redist/vcruntime140.dll" ]; then
  "$TC/python/python.exe" "$ROOT/scripts/fetch_vc_redist.py" "$(cygpath -w "$TC/vc_redist")"
fi

if [ "$DEBUG_TOOLS" = 1 ]; then
  if [ ! -f "$TC/procdump/procdump64.exe" ]; then
    fetch "https://download.sysinternals.com/files/Procdump.zip" "Procdump.zip"
    unzip_to "$DL/Procdump.zip" "$TC/procdump"
  fi
  if [ ! -f "$TC/renderdoc/renderdoccmd.exe" ]; then
    f="RenderDoc_${RENDERDOC_VER}_64.zip"
    fetch "https://renderdoc.org/stable/$RENDERDOC_VER/$f" "$f"
    rm -rf "$TC/RenderDoc_${RENDERDOC_VER}_64"
    unzip_to "$DL/$f" "$TC"
    mv "$TC/RenderDoc_${RENDERDOC_VER}_64" "$TC/renderdoc"
  fi
fi

rm -rf "$DL"
echo "[toolchain] installed in $TC:"
"$TC/llvm/bin/clang.exe" --version | head -1
"$TC/cmake/bin/cmake.exe" --version | head -1
echo "ninja $("$TC/bin/ninja.exe" --version)"
"$TC/python/python.exe" -c "import numpy, PIL, xxhash, Crypto, pytest; import sys; print('python', sys.version.split()[0], '+ modules OK')"
ls "$TC/vc_redist"
echo "[toolchain] done: prefix direct toolchain calls with: source scripts/dev_env.sh &&"
