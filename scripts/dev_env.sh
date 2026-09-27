#!/usr/bin/env bash
# Toolchain environment for every cmake/ninja/clang/python call (source it):
#   source scripts/dev_env.sh
# Kit scripts that need the toolchain source it themselves; a direct clang/cmake/python call
# from a fresh shell needs `source scripts/dev_env.sh && ...` (shell state does not persist).
# No admin rights and no Visual Studio: portable LLVM (clang, lld), MSVC CRT + Windows SDK
# headers/libs unpacked by xwin, portable CMake/Ninja/Python. Paths come from kit.env
# (relative = kit root; scripts/setup_toolchain.sh installs them in tools/toolchain/).

KIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
set -a; source "$KIT_ROOT/kit.env"; set +a

_kit_abs() { case "$1" in /*|?:*) printf '%s' "$1" ;; *) printf '%s' "$KIT_ROOT/$1" ;; esac; }
LLVM_DIR="$(_kit_abs "$LLVM_DIR")"; XWIN_SYSROOT="$(_kit_abs "$XWIN_SYSROOT")"
NINJA_DIR="$(_kit_abs "$NINJA_DIR")"; CMAKE_DIR="$(_kit_abs "${CMAKE_DIR:-}")"
PYTHON_DIR="$(_kit_abs "${PYTHON_DIR:-}")"; VC_REDIST_GLOB="$(_kit_abs "$VC_REDIST_GLOB")"
export LLVM_DIR XWIN_SYSROOT NINJA_DIR CMAKE_DIR PYTHON_DIR VC_REDIST_GLOB KIT_ROOT

# Kit-local tools first; sourcing twice does not grow PATH.
case ":$PATH:" in
  *":$LLVM_DIR/bin:"*) ;;
  *) export PATH="$LLVM_DIR/bin:$NINJA_DIR:$CMAKE_DIR:$PYTHON_DIR:$PYTHON_DIR/Scripts:$PATH" ;;
esac

if command -v cygpath >/dev/null 2>&1; then   # Windows (Git Bash)
  _w() { cygpath -w "$XWIN_SYSROOT/$1"; }
  INCLUDE="$(_w crt/include);$(_w sdk/include/ucrt);$(_w sdk/include/um);$(_w sdk/include/shared);$(_w sdk/include/winrt);$(_w sdk/include/cppwinrt)"
  LIB="$(_w crt/lib/x86_64);$(_w sdk/lib/ucrt/x86_64);$(_w sdk/lib/um/x86_64)"
  export INCLUDE LIB
  # No link.exe: lld for everything.
  case " ${CFLAGS:-} " in *" -fuse-ld=lld "*) ;; *) export CFLAGS="-fuse-ld=lld ${CFLAGS:-}" ;; esac
  case " ${CXXFLAGS:-} " in *" -fuse-ld=lld "*) ;; *) export CXXFLAGS="-fuse-ld=lld ${CXXFLAGS:-}" ;; esac
fi

[ -n "${KIT_ENV_QUIET:-}" ] || echo "[dev_env] clang: $(command -v clang || echo MISSING)  cmake: $(command -v cmake || echo MISSING)  ninja: $(command -v ninja || echo MISSING)  python: $(command -v python || echo MISSING)" >&2
