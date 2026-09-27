#!/usr/bin/env bash
# Toolchain environment for every cmake/ninja/clang call (source it):
#   source scripts/dev_env.sh
# No admin rights and no Visual Studio instance on this PC: portable LLVM (clang,
# lld), MSVC CRT + Windows SDK headers/libs unpacked by xwin, ninja from pip.
# Paths come from kit.env. See docs/TOOLCHAIN_SETUP.md for how each piece was obtained.

KIT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
set -a; source "$KIT_ROOT/kit.env"; set +a

export PATH="$LLVM_DIR/bin:$NINJA_DIR:$PATH"
export INCLUDE="$(cygpath -w "$XWIN_SYSROOT/crt/include");$(cygpath -w "$XWIN_SYSROOT/sdk/include/ucrt");$(cygpath -w "$XWIN_SYSROOT/sdk/include/um");$(cygpath -w "$XWIN_SYSROOT/sdk/include/shared");$(cygpath -w "$XWIN_SYSROOT/sdk/include/winrt");$(cygpath -w "$XWIN_SYSROOT/sdk/include/cppwinrt")"
export LIB="$(cygpath -w "$XWIN_SYSROOT/crt/lib/x86_64");$(cygpath -w "$XWIN_SYSROOT/sdk/lib/ucrt/x86_64");$(cygpath -w "$XWIN_SYSROOT/sdk/lib/um/x86_64")"
# No link.exe: lld for everything.
export CFLAGS="-fuse-ld=lld ${CFLAGS:-}"
export CXXFLAGS="-fuse-ld=lld ${CXXFLAGS:-}"

echo "[dev_env] clang: $(command -v clang)  ninja: $(command -v ninja)"
