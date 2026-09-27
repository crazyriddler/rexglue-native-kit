#!/usr/bin/env bash
# Local toolchain bootstrap for this machine (no admin rights, no Visual Studio installed).
#
# Provides a working clang/clang++ (LLVM 23.1.1 portable release) + lld linker +
# MSVC CRT / Windows SDK headers-and-libs (fetched via `xwin`, since the VS
# Installer never completed a usable instance on this machine and choco/winget
# both require elevation we don't have). Source this before any cmake/ninja
# invocation for the SDK build or the project build.
#
# See docs/port_status.md ("Phase 0 / Phase 1 environment notes") for how each
# piece was obtained and why it was necessary.

set -e

LLVM_DIR="/c/Users/jrbar/tools/clang+llvm-23.1.1-x86_64-pc-windows-msvc"
XWIN_SYSROOT="/c/Users/jrbar/tools/xwin_sysroot"
NINJA_DIR="/c/Users/jrbar/AppData/Roaming/Python/Python314/Scripts"

export PATH="$LLVM_DIR/bin:$NINJA_DIR:$PATH"

export INCLUDE="$(cygpath -w "$XWIN_SYSROOT/crt/include");$(cygpath -w "$XWIN_SYSROOT/sdk/include/ucrt");$(cygpath -w "$XWIN_SYSROOT/sdk/include/um");$(cygpath -w "$XWIN_SYSROOT/sdk/include/shared");$(cygpath -w "$XWIN_SYSROOT/sdk/include/winrt");$(cygpath -w "$XWIN_SYSROOT/sdk/include/cppwinrt")"
export LIB="$(cygpath -w "$XWIN_SYSROOT/crt/lib/x86_64");$(cygpath -w "$XWIN_SYSROOT/sdk/lib/ucrt/x86_64");$(cygpath -w "$XWIN_SYSROOT/sdk/lib/um/x86_64")"

# Force lld as the linker (no link.exe available) and target windows-msvc ABI,
# matching what the SDK's CMakePresets.json (clang/clang++ compiler) expects.
export CFLAGS="-fuse-ld=lld ${CFLAGS:-}"
export CXXFLAGS="-fuse-ld=lld ${CXXFLAGS:-}"

echo "[dev_env] clang:  $(which clang)"
echo "[dev_env] ninja:  $(which ninja)"
echo "[dev_env] INCLUDE set ($(echo "$INCLUDE" | tr ';' '\n' | wc -l) dirs)"
echo "[dev_env] LIB     set ($(echo "$LIB" | tr ';' '\n' | wc -l) dirs)"
