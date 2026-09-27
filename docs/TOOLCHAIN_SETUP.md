# Toolchain setup (no admin rights, no Visual Studio instance needed)

Everything is per-user. The locations below are the kit author's PC (September 2026): on
another PC install the same tools anywhere (the last column says how, without admin rights)
and set the paths in `kit.env`; `scripts/dev_env.sh` wires them together. Verify before
reinstalling.

| Tool | Location on the author's PC | How it was obtained (if it must be redone) |
|---|---|---|
| LLVM/Clang 23.1.1 (clang, clang++, lld, llvm-symbolizer) | `C:\Users\jrbar\tools\clang+llvm-23.1.1-x86_64-pc-windows-msvc` | Official portable release tarball `clang+llvm-<ver>-x86_64-pc-windows-msvc.tar.xz` (not the installer) |
| MSVC CRT + Windows SDK headers/libs | `C:\Users\jrbar\tools\xwin_sysroot` | `xwin` (github.com/Jake-Shadle/xwin) `splat --include-debug-libs`; one symlink error (os error 1314) is cosmetic |
| Ninja | `%APPDATA%\Python\Python314\Scripts\ninja.exe` | `python -m pip install --user ninja` |
| CMake >= 3.25 | on PATH | - |
| Python 3.14 + numpy, Pillow, xxhash, pycryptodome | on PATH | `pip install --user numpy pillow xxhash pycryptodome` |
| DXC v1.9.2607 | `tools/dxc` (local; gitignored) | `tools/shaders/build_corpus.sh` downloads it if missing |
| PowerPC binutils (objdump) | `tools/binutils` (in the kit) | - |
| ProcDump | `C:\Users\jrbar\tools\procdump` | Sysinternals zip |
| DbgEng debugger (`windbg-tool.exe`) | `C:\Users\jrbar\tools\windbgtool` | NuGet package `devolutions.windbg.tool.win-x64` (plain zip), MCP-style tools over dumps/live processes |
| RenderDoc 1.37 portable | `C:\Users\jrbar\tools\renderdoc` | Launch under `renderdoccmd capture` from process start (late inject does not hook D3D12) |
| Visual C++ redistributable DLLs (release folder) | `C:\Program Files\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT` | `kit.env VC_REDIST_GLOB` |

## SDK third-party code (fresh clone only)

`sdk/thirdparty/*/` (~400 MB of vendored dependencies, no submodules) is gitignored, so a
kit cloned from GitHub has only `sdk/thirdparty/CMakeLists.txt`. Run
`bash scripts/restore_sdk_thirdparty.sh` once: it fetches every entry at the commit the
SDK base (`c94f5eb`) pins, removes `.git` (vendored layout), fixes symlink placeholders on
Windows and applies the kit patches in `sdk/patches/thirdparty/` (today: the disruptorplus
`wait_for`/`wait_until` argument order, EXP-040). Existing non-empty entries are never
touched. A local kit that already has `sdk/thirdparty/` needs nothing.

## Build configurations

- `RelWithDebInfo` (dev build, `$BUILD_DEV`): logs, PDBs, profiling, A/B with Xenos.
  Debug executables cannot run here (no `ucrtbased.dll`).
- `Release` (`$BUILD_REL`): benchmarks and the release folder. Logging forced off.
- Game flags: `-march=x86-64-v3 -ffp-contract=off` (AVX2; `-ffp-contract=off` keeps the
  recompiled PPC float math unfused) + `cpu_check.cpp` compiled for baseline x86-64.
  The SDK subdirectory inherits the outer project's flags: without at least v2 its SSSE3
  helpers fail to compile.

## Configure (from $PORT_DIR, after `source ../scripts/dev_env.sh`)

```bash
C=$LLVM_DIR/bin
cmake -S . -B out/build/nr -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=$C/clang.exe -DCMAKE_CXX_COMPILER=$C/clang++.exe \
  "-DREXSDK_DIR=$(cygpath -m "$KIT_ROOT/sdk")" \
  "-DCMAKE_C_FLAGS=-march=x86-64-v3 -ffp-contract=off" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3 -ffp-contract=off"
# same with -B out/build/nr-rel -DCMAKE_BUILD_TYPE=Release
```

The SDK builds as a subdirectory of the game (REXSDK_DIR mode) and puts its outputs in
`sdk/out/win-amd64` (rexruntime.dll, rexgpu-xenos.dll, the `rexglue.exe` CLI).
`bench/build.sh [dir]` builds and syncs the DLLs. Re-run cmake when files are added or
when an embedded resource input (shader pack, pipeline base) appears for the first time.

## Environment traps

- The kit path must not break tools: prefer paths without spaces for new kits; the
  scripts already cope with spaces (relative dump dirs).
- A corrupted machine PATH entry with a stray quote breaks `vcvarsall.bat`-based builds
  (not used here). A broken Chocolatey python shim broke a Meson build once: remove it
  from PATH for that process only.
- Windows Developer Mode / symlink privilege is NOT available: use junctions and hard
  links for game-data aliases; never ask to change system settings.
