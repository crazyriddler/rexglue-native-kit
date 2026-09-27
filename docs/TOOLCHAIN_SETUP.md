# Toolchain setup (no admin rights, no Visual Studio, inside the kit)

`bash scripts/setup_toolchain.sh --accept-microsoft-license [--debug-tools]` installs every
build tool into `tools/toolchain/` (gitignored): pinned versions, SHA-256 checked, idempotent
(an installed component is kept; delete its folder to reinstall). `kit.env` points at these
folders by default (relative paths = kit root; an absolute `/c/...` path uses a toolchain
installed elsewhere) and `scripts/dev_env.sh` puts them first on PATH and sets INCLUDE/LIB.
Nothing outside the kit folder is touched (no PATH/registry change, no pip `--user`).

| Tool | Kit location | Source (pinned in setup_toolchain.sh) |
|---|---|---|
| Python 3.14.7 + `scripts/requirements-dev.txt` (numpy, Pillow, xxhash, pycryptodome, pytest, clang-format) | `tools/toolchain/python` | python-build-standalone 20260924 `install_only` |
| CMake 3.31.12 | `tools/toolchain/cmake` | Kitware portable zip (3.x: some vendored third-party CMake files predate CMake 4's policy floor) |
| Ninja 1.13.1 | `tools/toolchain/bin/ninja.exe` | ninja-build release zip |
| LLVM/Clang 23.1.1 (clang, clang++, lld, llvm-rc, llvm-symbolizer) | `tools/toolchain/llvm` | official portable tarball `clang+llvm-23.1.1-x86_64-pc-windows-msvc.tar.xz` (~900 MB, no symlinks inside) |
| MSVC CRT + Windows SDK headers/libs (Microsoft license) | `tools/toolchain/xwin_sysroot` (`crt/`, `sdk/`) | xwin 0.10.0 `splat --include-debug-libs --disable-symlinks` (symlinks need a privilege users lack; NTFS is case-insensitive) |
| Visual C++ runtime DLLs for the release folder (Microsoft license) | `tools/toolchain/vc_redist` (+ `VERSION.txt`) | `scripts/fetch_vc_redist.py`: newest x64 CRT redist package of the VS 2022 channel manifest (the one xwin uses), SHA-256 checked |
| ProcDump, RenderDoc 1.37 portable (`--debug-tools`) | `tools/toolchain/procdump`, `tools/toolchain/renderdoc` | Sysinternals zip; renderdoc.org portable zip. Launch RenderDoc under `renderdoccmd capture` from process start (late inject does not hook D3D12) |
| DXC v1.9.2607 | `tools/dxc` (gitignored: Microsoft's license on `dxil.dll` does not allow redistributing it in the repository) | `tools/shaders/build_corpus.sh` downloads it automatically if missing |
| PowerPC binutils (objdump) | `tools/binutils` (in git) | - |
| DbgEng debugger (optional) | anywhere | NuGet package `devolutions.windbg.tool.win-x64` (plain zip): MCP-style tools over dumps/live processes; not installed by the script |

Validation status: the pinned URLs, hashes and archive layouts were checked on 2026-09-27;
the script has not yet run end to end on Windows, and xwin/`fetch_vc_redist.py` could not be
exercised from the cloud (Microsoft hosts blocked there). If a step fails, fix the script
(record it in LESSONS_LEARNED A) rather than installing by hand.

## SDK third-party code (in the repository)

`sdk/thirdparty/*/` (~400 MB, no submodules) is committed: every entry at the commit the SDK
base (`c94f5eb`) pins, without `.git`, symlinks replaced by real files, kit patches from
`sdk/patches/thirdparty/` already applied (today: the disruptorplus `wait_for`/`wait_until`
argument order, EXP-040). `sdk/thirdparty/.gitattributes` (`* -text`) keeps the exact upstream
bytes on every platform (the libraries' own `.gitattributes` are kept as
`.gitattributes.upstream`). A clone needs nothing else.
`bash scripts/restore_sdk_thirdparty.sh [name,...]` re-vendors an entry that was deleted
(fetch at the base pin + kit patch); existing non-empty entries are never touched.

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
