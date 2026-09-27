# Project State

> Handoff between sessions. Keep current. Last update: 2026-09-26 (optimization round for the beta, EXP-047).

## Current status
- Phase: M0-M7. The native renderer draws every frame of the covered content and matches Xenos frame-exactly:

  | Content | PSNR |
  |---|---|
  | jungle / combat (Chance Meeting save) | 50-51 dB |
  | new game prologue level (cinematic, gameplay, HUD, tutorials) | 47.6-50.8 dB |
  | menus | 58 dB |
  | FMV intros | 50-58 dB |

  See EXP-032/033.
- Performance: native 4.23 ms vs legacy 4.77 ms (Release, 1000 Hz vblank, EXP-030; recording worker). Stable (own D3D12 queue EXP-026, hang watchdog).
- **Graphics enhancements (EXP-045/046)**, all optional in the launcher's Enhancements group (off = original):
  - full-resolution scene, foliage alpha-to-coverage, FXAA, SSAO (camera from g_mProjectionToWorld), bloom quality, dithering;
  - shadow smoothing (rotated PCF), soft particles.
  - Always on since EXP-048 (fixes, not options): full-resolution scene, smooth effects above 720p. MSAA is a launcher option (Off / 4x original / 8x); foliage AA is greyed out when MSAA is off.
  - Debug: `--ssao_debug`, `--native_shadow_pcf_mode`, `--native_gpu_pass_timing`.
- **Alpha release (EXP-037)**: `bash tools/make_release.sh [name]` builds Release and writes `release/<name>/`, a portable folder with conan.exe (shaders embedded), rexruntime.dll, VC++ runtime, conan.cfg and data/.
  - Settings live in `conan.cfg` and are edited by the startup launcher (localized EN/ES/FR/DE/IT). Defaults are the original game.
  - Graphics options: render_scale 1-4, shadow_quality 1-3, msaa_samples, anisotropic_filtering, fps_limit, vsync, fullscreen/window.
  - Benchmarks: `bench/run.sh` forces scale 1 / fps_limit 0 / vsync off / no launcher unless overridden. Never pass a flag twice: the parser then drops all flags.
- **Fully native build (EXP-036)**: `native_renderer` now defaults to true, and the Xenos GPU plugin is no longer loaded. `conan::native::NativeGraphicsSystem` (conan-port/src/native/native_graphics_system.*) provides the D3D12 presenter, GPU MMIO registers, a sync-only PM4 consumer (fences, interrupts, swap counter, gamma), and vblank.
  - Legacy Xenos emulation: `--native_renderer=false`.
  - Plugin sync-only mode: `--native_graphics_system=false`.
  - A/B (`native_ab_mode`) still loads Xenos as its reference.
- Guest-bound ceiling (no Xenos translation, 1000 Hz vblank): 3.97 ms/frame.
- **EXP-047 (beta optimization round)**: jungle unlocked 4.28 -> ~4.05 ms. The game build targets x86-64-v3 (AVX2) with `-ffp-contract=off`; src/cpu_check.cpp shows a message on older CPUs. `bench/opt_baseline.sh <tag>` measures frame time, CPU/power at 60 fps, RAM and VRAM.

## Workspace
- Root `C:\Users\jrbar\rexglue-sdk native-render` = ReXGlue SDK v0.10.0 fork, local git branch `native-render`.
- Game project `conan-port/` (own nested git repo, branch master) - hooks & native renderer live in `conan-port/src/native/`.
- `../rexglue-sdk-oldperf` = git worktree of pre-perf SDK (c94f5eb + perf-backup patch + bench harness) used for EXP-005 bisection; build dir conan-port/out/build/nr-old.
- NEVER touch `C:\Users\jrbar\rexglue-sdk` (original SDK), `C:\Users\jrbar\Documents\conan` (user saves) or `conan-port/dist` (user's Release build).

## Build
```bash
bench/build.sh nr-rel   # Release (benchmarks); bench/build.sh nr = RelWithDebInfo (symbols/profiling)
```
Always use bench/build.sh: it syncs SDK DLLs into the exe dir (CMake only copies them when conan.exe relinks - stale DLL trap). After adding source files: `cd conan-port && source scripts/dev_env.sh && cmake out/build/nr-rel`.

## Run / benchmark (automated, no focus needed)
```bash
EXE_DIR="$PWD/conan-port/out/build/nr-rel" bench/run.sh <name> bench/scenario_jungle.txt 92 [--cvar=value ...]
python tools/bench_summary.py artifacts/profiles/<name>.csv 100 50 90
```
- Plugin cvars need `--name=value`. Benchmark window t=50..90 s. Warm shader cache in bench/userdata_template/cache (copied fresh per run).
- Useful cvars: `--bench_screenshot_times=50,70` (BMP to artifacts/screenshots/<name>/), `--sample_profile_out=<file>` (+ tools/symbolize_profile.py; use nr build for symbols), `--d3d_capture_out=<json>`, `--native_dump_textures_dir=<dir>`, `--native_renderer=true`, `--gpu_null_draws=true`, `--unlocked_vblank_rate=N`.
- Two path clusters exist for the scripted walk (~1200 draws vs ~1350 draws); compare within cluster.

## Key findings (details in EXPERIMENT_LOG)
- EXP-005: cold shader cache -> black scene (async pipeline skip dropped a one-time feedback draw). Fixed: wait for pending pipelines.
- EXP-007: host Present blocked the D3D12 queue lock -> CP stalls quantized to 280 Hz. Fixed with frame-latency waitable swap chain (-32..41%).
- EXP-008: guest issues 616 draws/frame, 29 resolves, 17 passes; Xenos processes 1209 (tiling replays).
- Game waits 2 vblanks/frame: vsync=true -> 30 fps cap; unlocked 240 Hz -> 120 fps cap.

## Architecture (see RENDERER_ANALYSIS.md, NATIVE_RENDERER_DESIGN.md, UPSTREAM_RESEARCH.md)
- Device ptr 0x82C81A64 (0x5700 bytes). Register shadow mapping in RENDERER_ANALYSIS section 8.
- Draws: 82580918 / 82580D00 / 825808B8 / BeginVertices 825803F8 (HUD). Resolve 822F5028, Swap 822E8EB8, pass runner 824F1E70 (pass fns hooked in d3d_capture.cpp).
- Shaders: artifacts/shaders/catalog.json + dxil/ (591/591 compile, `bash tools/shaders/build_corpus.sh`), keyed by container_hash; not yet validated visually.
- Native threading (EXP-030): the hooks (guest threads) only capture. Each hook builds a WorkCmd: PM4 words since the last command, device/object ranges, and buffer plans from the guest-side dirty tracker. Batches of 32 go to the recording worker (`Renderer::WorkerMain`). The worker's Load32 reads the captured ranges first. Swap waits for the previous frame (`native_worker_lag`). `native_worker=false` runs the same commands synchronously (debug).
- Hang watchdog (`native_hang_watchdog_s`, default 6): logs the stacks of every thread when swaps stop.
- Stack profiles: `--sample_profile_out` + `tools/profile_stacks.py <file> <slot> <func>`. Slots: 0 main/swap, 1 Xenos CP, 2 present, 3 game render thread, 4 native worker.

## Native renderer status (2026-09-25, late)
| Area | Native | Validated | Notes |
|---|---|---|---|
| Frame structure (passes, tiling, resolves, clears) | yes | trace + A/B | tiling resolves in place, clears limited to the resolve rect, EDRAM aliasing model |
| Constants / fetch constants | PM4 mirror | == Xenos register file | copy state captured at copy draws (copy_dest_swap) |
| Textures | CPU decode + cache + write-watch revalidation | fog table, cubes, grass, FMV, HUD atlases | PWL gamma in shader; 3D textures base level only; 64bpp Xenos readbacks unusable for A/B |
| Shaders | XenosRecomp DXIL (591) | whole scene | runtime spec constants (alpha test), precise oPos, g_ScreenXform |
| Shadows / depth resolves | shader copy (R32F + raw D24S8 RGBA8 view) | atlas + bloom mask | |
| Post / present | yes | A/B | display gamma ramp applied in the blit |
| Full frame | yes | A/B 50-51 dB jungle/combat, 58 dB menus (EXP-032, practically pixel-exact) | PWL gamma, MSAA + alpha to coverage, rect lists, Xenos pixel centers |
| Performance | 4.22 ms (EXP-030, legacy 4.77) | vs legacy 4.77 ms | CPU-bound on the guest render thread |

## Next actions (in order)
1. Performance: with the plugin gone (EXP-036) native runs at 4.23 ms vs legacy 4.77 ms; the guest-only ceiling is 3.97 ms. The remaining guest-thread cost is capture/planning (~6-8% of the render thread: CaptureDevice, PlanStreams). Next: cut that capture cost, and check whether the native CP thread could be folded into the swap hook.
   - Visual A/B still needs the plugin (`native_ab_mode`).
   - Validate `resolution_scale` > 1 on the native path (launcher option).
2. Load freeze after `AudioSystem::SubmitFrame ... dropping frame`: FIXED (E036 / EXP-034; the audio render client was unregistered while its callback was running; UnregisterClient now waits). `bench/repro_freeze.sh <runs> <scenario>` for regressions; the hang watchdog (`native_hang_watchdog_s`) still logs all thread stacks if swaps stop.
3. 3D textures: mips not decoded (base level only).
4. Broader coverage: covered = jungle save, new game prologue, FMV, menus (`bench/ab_multi.sh`). Other levels need other saves (the user's saves folder is off-limits); play further into the prologue with longer scripts. Long session: bench/scenario_long.txt (10 min pseudo-random walk/fight).
5. Retire experiment cvars (the unconditional `native dbg` logs were removed in EXP-047).

## A/B workflow (frame-exact)
```bash
# swap numbers: grep "swap N" in the native log line "native: t=..s swap N"
N=abs1; S=4140
bench/run_safe.sh 200 $N bench/scenario_jungle.txt 85 --native_renderer=true --native_ab_mode=true   --native_dump_swap=$S --native_dump_dir=../../../../artifacts/captures/$N --bench_screenshot_swaps=$S   [--d3d12_readback_resolve=true]
python tools/native_dump_to_png.py artifacts/captures/$N
python tools/ab_diff.py artifacts/captures/$N/output_1280x720.png artifacts/screenshots/$N/swap_00$S.bmp
# or simply: bench/ab_swap.sh <name> <swap> <exit_s> [cvars]
python tools/ab_resolves.py artifacts/captures/$N out.png
```
Dump dirs must be relative (the repo path contains a space). bench/run_safe.sh kills hung runs.

Multi-swap A/B (preferred): `bench/ab_multi.sh <name> <scenario.txt> <exit_s> <swap,swap,...> [cvars]` -> PSNR per swap + artifacts/captures/<name>_sheet.png (native | xenos | diff x8). Scenarios: scenario_jungle, scenario_combat, scenario_newgame, scenario_fmv, scenario_long.

## Debug workflow for the native path
- `--native_trace_swap=N`: full trace of frame N (per draw: shaders, mirror fetch constants per slot + bound host resource, viewport/VTE/scissor, stencil/depth/poly offset, VS/PS c160-163, inline vertices hex). Pass numbers come from the guest pass hooks (g_guest_pass).
- `--native_debug_ps_const=<ps hash>:<reg>:x,y,z,w`, `--native_skip_draws=<pass>:<first>-<last>`, `--native_skip_ps=<hash>`: bisection.
- `--native_debug_texture_format=<xenos fmt>`: log creations/revalidations of that format, and dump them (DDS at creation; raw at the dump swap).
- `--native_renderer=true --native_dump_frame_at_s=60 --native_dump_dir=<dir>` then `python tools/native_dump_to_png.py <dir>`.
- `--native_pass_mask=8,12` renders only those passes; `--native_draws=false`, `--native_resolves=false`, `--native_debug_resolve_mode=1|2` for bisection.
- `--d3d12_debug=true` logs debug-layer messages into the game log.
- `--native_trace_swap=N` / `--native_trace_frame_at_s=T`: log RT binds, resolves (with RB_COPY_DEST_INFO), per-draw shaders/state/fetch constants for one frame. `--native_dump_after_pass=99`: surfaces after every pass (tools/dump_sheet.py). GPU stalls print breadcrumbs (last completed draw) on fence timeout.
- `--gpu_trace_register=<index>` (SDK CP): which PM4 packets write a register.
- RelWithDebInfo build (bench/build.sh nr) for logs; Release (nr-rel) for timing. Hung runs leave conan.exe alive: `taskkill //F //IM conan.exe`.
