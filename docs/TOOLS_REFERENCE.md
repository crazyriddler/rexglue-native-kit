# Tools reference

All scripts read `kit.env` (bash: `source`, Python: `tools/kitcfg.py`). Run from the kit root
in Git Bash unless noted.

## bench/ (running the game)

| Script | Use |
|---|---|
| `bench/build.sh [builddir]` | build game+SDK and sync SDK DLLs into the exe dir (always use it) |
| `bench/run.sh <name> <script.txt> <exit_s> [--cvar=v ...]` | one scripted run: fresh bench user data, autoinput, perf CSV `artifacts/profiles/<name>.csv`, screenshots dir; `EXE_DIR=` picks the build |
| `bench/run_safe.sh <limit_s> <run.sh args>` | run.sh with a wall-clock limit; kills only its own process |
| `bench/ab_multi.sh <name> <scenario> <exit_s> <swaps> [cvars]` | frame-exact native vs Xenos at several swaps -> PSNR + contact sheet |
| `SCEN=... bench/ab_swap.sh <name> <swap> <exit_s>` | one swap with full dumps |
| `bench/repro_freeze.sh <runs> <scenario> [cvars]` | hang rate with the watchdog |
| `SCEN_A=.. SCEN_B=.. bench/opt_baseline.sh <tag>` | frame time (unlocked), hitches, CPU/RAM/GPU power at 60 fps, VRAM |
| `bench/scenario_*.txt` | autoinput scripts: `<start_ms> <hold_ms> <BUTTON>[+BUTTON]`; buttons A B X Y START BACK UP DOWN LEFT RIGHT LB RB LT RT, sticks LSU/LSD/LSL/LSR RSU/RSD/RSL/RSR; timed from the guest's first input poll. Examples: reference/conan/bench/ |
| `bench/userdata_template/` | the bench user data root (saves, caches) copied fresh per run |

## tools/ (analysis)

| Tool | Use |
|---|---|
| `bench_summary.py <csv> [window_s] [from_s] [to_s]` | one line per `window_s` slice of [from_s, to_s) plus a TOTAL line: frame time avg/p50/p95/p99 + per-frame counters (e.g. `... 40 50 90` = one 50-90 s line + TOTAL) |
| `thread_cpu.py <secs> [exe] [top]` / `thread_cpu.ps1` | per-thread CPU (names) of the running game |
| `power_probe.py <secs> <delay> <out>` | process CPU + NVIDIA GPU power/util |
| `mem_probe.ps1 <delay>` | RAM working set/private |
| `symbolize_profile.py`, `profile_stacks.py <prof> <slot> <func>`, `profile_threads.py` | SDK sampler profiles (`--sample_profile_out`, `--sample_profile_all_threads`) |
| `native_dump_to_png.py <dir>` | native .raw dumps -> PNG |
| `ab_diff.py`, `ab_multi.py`, `ab_resolves.py`, `dump_sheet.py`, `grid.py` | image comparison / contact sheets |
| `capture_window.py <exe> <class> <out.png> [--close] [--open-combo=id] [args]` | capture a window of a process it starts (launcher screenshots) |
| `stfs_extract.py [--info] [--header f --file-name n] <package> <out_dir>` | extract Xbox 360 STFS packages (CON/LIVE/PIRS: saves, DLC) into the folder layout the content manager expects (`<xuid>/<title>/00000001/<name>/` + `Headers/00000001/<name>.header`). Saves from other regions/editions of the same game usually load |
| `make_release.sh [name]` | portable release folder |
| `make_pipeline_base.sh name:secs ...` | record PSOs over scenarios and MERGE into artifacts/shaders/pipelines_base.bin |
| `shaders/build_corpus.sh` | full shader pipeline (docs/SHADER_PIPELINE.md) |
| `shaders/gen_projection_regs.py` | per-shader register of the camera matrix constant (edit the constant name per engine) |
| `re/xdk_sigs.py build|match` | XDK D3D function signatures (docs/GAME_ADAPTATION_GUIDE.md) |
| `re/xdk_layout.py <register.cpp> [--image] [--lib d3d9i,LIBCMT,xapilibi]` + `re/xdk_2012_dc3_symbols.tsv` | name XDK D3D/CRT/xapilib functions (incl. setjmp/longjmp, memcpy, heap for [rexcrt]) by function-size layout for late-XDK games; reference = Dance Central 3 (2012, CC0 dc3-decomp). Test: `python -m pytest scripts/tests/test_xdk_layout.py` |
| `re/disdb.py build`, `re/q.py dis|callers|callees|grep`, `re/gref.py`, `re/pm4scan.py`, `re/ptrtables.py`, `re/callargs.py`, `re/constwriters.py`, `re/fingerprint.py`, `re/apisurface.py`, `re/listrange.py` | generic disassembly analysis (set `CODE_START` to REX_CODE_BASE) |
| `re/names.py`, `re/statetables.py`, `re/passtable.py`, `re/gamelayer.py`, `re/d3d_symbols.tsv` | Conan-specific names/table addresses: use as templates, update the addresses for the new game |
| `binutils/powerpc-none-elf-objdump.exe` | PPC disassembler |
| `dxc/` | DirectX Shader Compiler |

## scripts/

| Script | Use |
|---|---|
| `setup_toolchain.sh --accept-microsoft-license [--debug-tools]` | install the whole toolchain into `tools/toolchain/` (LLVM, xwin CRT/SDK, CMake, Ninja, Python + modules, VC++ runtime; docs/TOOLCHAIN_SETUP.md) |
| `fetch_vc_redist.py <dir>` | VC++ runtime DLLs from the VS 2022 manifest (called by setup_toolchain.sh) |
| `dev_env.sh` | toolchain environment: kit.env paths (relative = kit root) first on PATH, INCLUDE/LIB, lld; kit scripts source it, direct calls use `source scripts/dev_env.sh && ...` |
| `restore_sdk_thirdparty.sh [names]` | re-vendor missing/empty `sdk/thirdparty/` entries at the SDK base pins + kit patches (the entries are in git; docs/TOOLCHAIN_SETUP.md) |
| `check_vulkan_stack.py` | SDK Vulkan submodule pins; in the kit (vendored SDK) it only reports that pins are not checkable |
| `tests/` (`python -m pytest scripts/tests`) | codegen semantics (sraw/srad), xdk_layout.py, check_vulkan_stack |
| `port/xex_decode.py <xex> <out.bin>` | decrypt + decompress (basic compression) to a flat image at 0x82000000 |
| `port/find_cross_file_gotos.py <generated dir>` | fall-through functions split across files |
| `port/find_adjustor_thunks.py [--toml] [dis reg.cpp]` | every MSVC adjustor thunk (`addi r3,r3,-N; b`) codegen did not register, ranked high (packed run) / check (lone); `--toml` prints `[entrypoint.functions]` entries. Test: `scripts/tests/test_find_adjustor_thunks.py` |
| `port/find_unregistered_after_bctr.py` | candidates for unregistered indirect-call targets (edit its two paths) |
| `port/fix_broken_symlinks.py <git dir>` | text-file symlinks from Git without privilege |
| `port/tga_to_dds.py` | convert an existing TGA asset to DDS for a loader that wants .dds |
| `port/symbolize_offline.ps1`, `port/sample_thread_rip.ps1` | offline RIP symbolization / thread RIP sampling |
| `port/input_helpers.ps1` | dot-source for real key/mouse injection into the game window and window screenshots (own process by GAME_NAME); prefer `--autoinput_script` for automation |
| `ida/export_named_funcs.py` | IDA script: export named functions into the ReXGlue manifest's function TOML (keeps existing entries' metadata) |
| `new_experiment.ps1 -Title <t>` | append the next numbered EXP entry template to docs/EXPERIMENT_LOG.md |
| `init_local_git.ps1` | `git init` the kit root if needed (no remote) |
| `collect_environment.ps1` | write a report of the installed tools and versions |
| `verify_local_workspace.ps1` | check the kit control files, Claude Code and git are present |
| `check_kit.ps1` | after phase 0: check the per-game state docs exist (fails on a fresh kit by design) |
| `fetch_upstream_references.ps1` | shallow-clone the surveyed projects into `_research/upstream/` |
| `requirements-dev.txt` | Python modules for the tools and tests (installed into the kit Python by setup_toolchain.sh) |
| `PSReX/`, `vs/rexglue-devprompt.*`, `git/hook-pre-commit.ps1` | ReXGlue SDK maintainer tooling (Visual Studio developer shell, clang-format pre-commit hook); not used by the kit workflow, which builds with `dev_env.sh` + `bench/build.sh` |

## SDK cvars added by this fork (useful everywhere)

`--autoinput_script`, `--perf_log_csv`, `--bench_exit_after_s`, `--bench_screenshot_times`,
`--bench_screenshot_swaps`, `--bench_screenshot_dir`, `--sample_profile_out`,
`--sample_profile_all_threads`, `--gpu_null_draws`, `--unlocked_vblank_rate`, `--vsync`,
`--dump_xex_image`, `--audio_debug_callback_delay_ms`, `--audio_unregister_wait`,
`--d3d12_readback_resolve`, `--mnk_mode`, `--user_data_root`, `--game_data_root`,
`--log_level`. A cvar passed twice drops all flags.
