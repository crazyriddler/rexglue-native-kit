# Native port playbook: Xbox 360 XEX -> native PC port with a native D3D12 renderer

The proven route (Conan 2007: ~3 weeks of agent time to a beta with native renderer,
launcher and optimized release). Each phase has exit criteria; do not skip ahead, but
parallelize research when useful. Before each phase read the matching section of
docs/LESSONS_LEARNED.md. Record every experiment in docs/EXPERIMENT_LOG.md.

Layout of the kit (the kit folder IS the project):
```text
kit.env                 game name, port dir, title id, toolchain paths, bench defaults
sdk/                    our ReXGlue v0.10.0 fork (native-render branch, vendored submodules)
game/                   <- the user drops the game here (default.xex + data), no recompilation
port/                   created in phase 0 by `rexglue init` (manifest, src/, generated/)
bench/                  run/run_safe/build/ab/repro/opt scripts + scenario_*.txt + userdata_template/
tools/                  bench/profiling/capture tools, shaders/ pipeline, xenosrecomp/, re/, dxc, binutils
scripts/                dev_env.sh (toolchain), port/ (xex decode, codegen scanners, symlink fix...)
reference/conan/        the complete worked example: port sources, manifest, docs, logs
docs/                   this knowledge base + templates/ for the per-game state files
artifacts/              captures, profiles, screenshots, shaders (generated)
release/                portable release folders (generated)
_research/upstream/     local mirrors: XenosRecomp, reblue(-XenosRecomp), UnleashedRecomp, plume, skate3recomp
```

---

## Phase 0 - Bootstrap (hours)

1. `source scripts/dev_env.sh`; verify clang/ninja/cmake/python (docs/TOOLCHAIN_SETUP.md).
2. Inventory `game/`: default.xex (+ any .xex/.dll modules), size, SHA-256 of the XEX
   (record it; never modify the original - copy to patch). Title ID from the XEX header
   (or later from the log).
3. Choose GAME_NAME (lowercase, no spaces) and fill `kit.env` (GAME_NAME, TITLE_ID,
   GUEST_WIDTH/HEIGHT when known).
4. Build the rexglue CLI: `cd sdk && cmake --preset win-amd64 && cmake --build
   out/build/win-amd64 --config Release --target rexglue` -> `sdk/out/win-amd64/Release/rexglue.exe`
   (~5 min; the first `codegen` of Conan gave exactly its 3 known UnresolvedCall errors).
5. `mkdir port && mv game port/game` (same volume: instant; leave `game/README.md` behind
   or recreate it), then from `port/`:
   `../sdk/out/win-amd64/Release/rexglue.exe init --project-name $GAME_NAME --xex-path
   game/default.xex --game-root game --project-root . [--scan-dll]` (paths relative to
   the project root; creates the manifest, CMakeLists.txt, CMakePresets.json, src/).
   Codegen later: `../sdk/out/win-amd64/Release/rexglue.exe codegen <game>_manifest.toml`
   (also exposed as the `<game>_codegen` CMake target).
6. Copy docs/templates/* to docs/ (PROJECT_STATE, EXPERIMENT_LOG, OPEN_QUESTIONS,
   BENCHMARKS.csv, RENDERER_ANALYSIS, SHADER_CATALOG, PIPELINE_CATALOG) and
   `port/docs/error_log.md`. `git init` the kit root (the .gitignore excludes game data,
   builds, artifacts).
7. Decoded image + disassembly for analysis: `python scripts/port/xex_decode.py
   port/game/default.xex port/logs/default_image.bin` (basic compression only; otherwise
   wait for phase 2 and use `--dump_xex_image`), then objdump (GAME_ADAPTATION_GUIDE.md §1).

Exit: kit.env filled, project initialized, state docs exist, git checkpoint.

## Phase 1 - Codegen to zero errors (hours - 1 day)

1. `rexglue codegen port/<game>_manifest.toml --log_file port/logs/codegen1.txt` (no --force).
2. For each UnresolvedCall: classify (import / undiscovered code / tail call into another
   function / data / jump table) with the disassembly; fix in the manifest with evidence
   comments (LESSONS_LEARNED B). One root-cause class per iteration.
3. `python scripts/port/find_cross_file_gotos.py port/generated/default` before building.

Exit: 0 analysis errors without --force; manifest overrides each commented with evidence.

## Phase 2 - Build and boot to a first frame on the legacy Xenos path (1-2 days)

1. CMakeLists: `rexglue_setup_target(<game> GPU_PLUGINS xenos)`, link `imgui::imgui`,
   `d3d12 dxgi dbghelp`, xxHash include (copy reference/conan/port/CMakeLists.txt shape).
   Presets/flags: docs/TOOLCHAIN_SETUP.md. Configure `nr` (RelWithDebInfo) and `nr-rel`.
2. App class (copy reference conan_app.h shape): OnConfigurePaths (data discovery,
   Release logging off), OnPreSetup (gpu_plugin default xenos), GetWindowTitle.
3. `bench/build.sh`, run with `--mnk_mode`; logs in `port/out/build/nr/logs/`.
4. Dump the image now if phase 0 could not: `--dump_xex_image=../../../logs/default_image.bin`.
5. Crash loop (LESSONS_LEARNED C): unregistered function -> manifest; guest AV -> symbolize
   (add the crash-diagnostics patch if needed), read the generated code, guard with a
   midasm hook to an existing bail-out, or supply a missing asset. One fix per run.
6. Automate input: `bench/scenario_*.txt` for `--autoinput_script` (look at
   reference/conan/bench/*.txt for the syntax: waits, key presses, holds). Create the
   benchmark save inside `bench/userdata_template` by playing the scenario, never from the
   user's own saves folder.

Exit: boots, menus, new game, gameplay, save/load, FMV work on the legacy path; scripted
scenarios reach gameplay reproducibly.

## Phase 3 - Baseline (hours)

Scenario, 3 runs, legacy Release: frame time distribution, per-thread CPU, GPU time,
draws/resolves/passes (`--d3d_capture_out`), null-draw ceiling. Write BENCHMARKS.csv and
the first EXPERIMENT_LOG entries. Apply the SDK-level legacy fixes if the kit SDK lacks one.
Finish with the bound classification (PERFORMANCE_GUIDE.md §"Where is the frame bound?"):
record the guest ceiling; it decides which optimizations are worth anything later.

## Phase 4 - Renderer archaeology + capture (1-3 days)

1. `tools/re/xdk_sigs.py match` -> XDK function table; confirm each hook target.
2. Device pointer, front buffer, present function, render-pass structure (pass table or
   markers), scene/output resolution, shadow passes (GAME_ADAPTATION_GUIDE.md §2).
3. Capture hooks (call originals): per-frame draws/resolves/passes catalog.
4. docs/RENDERER_ANALYSIS.md with addresses, evidence and confidence.

## Phase 5 - Shader corpus (hours)

`bash tools/shaders/build_corpus.sh` -> expect ~100% translate+compile with the patched
XenosRecomp; fix new translator failures in `tools/xenosrecomp/src` and regenerate the
patch (`git diff > ../patches/0001-conan-recomp.patch` from src/). Wire the DXIL pack
into CMake (RCDATA 2).

## Phase 6 - Native renderer bring-up (1-2 weeks)

Port `reference/conan/port/src/native/` (GAME_ADAPTATION_GUIDE.md): shader registry,
capture hooks, PM4 mirror, texture decode, renderer, worker, A/B mode. Order:
HUD/2D -> post chain -> scene passes -> shadows -> MSAA/alpha-to-coverage -> fidelity
(pixel centers, gamma, rect lists, copy swap). Validate every step with
`bench/ab_multi.sh` (VALIDATION_GUIDE.md) until 45-58 dB across scenarios.
Add the cheap D3D12 hardening (NATIVE_RENDERER_ARCHITECTURE.md §8: DRED, PIX pass markers,
NOT_ZEROED, root signature 1.1) and respect the invariants of §7 (never reorder, drop or
cull submitted draws).

## Phase 7 - Fully native (days)

NativeGraphicsSystem (no plugin): sync-only PM4 consumer, fences, vblank, gamma. Hang
watchdog on. Scenario sweep with 0 hangs (`bench/repro_freeze.sh`). Default to native;
A/B mode kept for regressions.

## Phase 8 - Performance (days)

PERFORMANCE_GUIDE.md checklist: worker thread, buffers/textures/constants caching,
busy-wait removal (fps cap CPU), PSO precompile + embedded base, x86-64-v3, memoized hot
guest functions if profiled. Measure every step (opt_baseline.sh, paired runs).
Then re-classify the bound and apply only the gated optimizations whose gate is met
(PERFORMANCE_GUIDE.md §"Gated optimizations"). On a guest-bound game (the usual case):
G1 codegen register-locality flags -> G2 guest XDK D3D cost -> G11 PGO. Renderer/GPU-side
items (resolve elision, vertex fetch in shader, parallel recording) only when the
classification says renderer- or GPU-bound.

## Phase 9 - Options, enhancements, launcher (days)

RELEASE_AND_SETTINGS.md: cfg + launcher; render resolution (arbitrary), shadow quality,
MSAA, anisotropy, FPS cap, VSync; fixes always on at higher resolutions (full-resolution
scene, smooth effects, upscaled-resolve sampling, shadow kernel scaling); enhancements
(foliage A2C, FXAA, SSAO, bloom quality, dithering, shadow smoothing, soft particles).
Validate each at scale 1 (A/B unchanged) and visually at 1440p/4K.

## Phase 10 - Release

Pipeline base from all scenarios, release folder, smoke test from the folder, long
session, launcher screenshots in EN/ES, docs/PROJECT_STATE.md final, git tag.
Release gates: VALIDATION_GUIDE.md §"Release gates" (A/B, 0 PSOs compiled in play,
0 hitches outside loads, 0 hangs, CPU at the cap, clean debug layer).

---

## Working rules that saved the most time

- Evidence before fixes: disassembly, generated code, runtime values. Hypotheses are
  labeled as such.
- One root-cause class per iteration; rebuild and rerun after each.
- A/B frame-exact against Xenos for every renderer change; numbers in the log.
- Keep diagnostics cvar-gated; remove investigation instrumentation from hot paths.
- Never kill the user's game processes, never touch the user's saves or release folders.
- Commit to local git at each milestone (kit root; the port may be its own repo).
