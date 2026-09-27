# Native port playbook: Xbox 360 XEX -> native PC port with a native D3D12 renderer

The single procedure of the kit: for each phase, **what to do (with the commands), what to
decide from what you observe, what not to do, and when the phase is done**. Cross-phase
guidance (work loop, priorities, symptom router, where to record, document map) is in
docs/DECISION_GUIDE.md; engineering rules and user preferences are in CLAUDE.md.

Proven on Conan (2007): about 3 weeks of agent time to a beta with native renderer, launcher
and optimized release. Do not skip a phase's exit criteria; research for a later phase may
run in parallel. Before a phase, read its LESSONS_LEARNED section.

## Layout (the kit folder IS the project)

```text
kit.env                 game name, port dir, title id, toolchain paths, bench defaults
sdk/                    ReXGlue v0.10.0 fork (base c94f5eb + sdk/KIT_SDK_CHANGES.md);
                        thirdparty/ vendored, gitignored (fresh clone: scripts/restore_sdk_thirdparty.sh)
game/                   <- the user drops the game here (default.xex + data)
port/                   created in phase 0 by `rexglue init` (manifest, src/, generated/, docs/error_log.md)
bench/                  run/run_safe/build/ab/repro/opt scripts, scenario_*.txt, userdata_template/
tools/                  bench/profiling/capture tools, shaders/, xenosrecomp/, re/ (xdk_sigs, xdk_layout), dxc, binutils
scripts/                dev_env.sh, restore_sdk_thirdparty.sh, port/ (xex decode, codegen scanners), tests/
reference/conan/        the complete worked example (sources, manifest, docs, logs) - read-only
docs/                   knowledge base + templates/ for the per-game state files
artifacts/, release/    generated (captures, profiles, shaders; portable release folders)
_research/upstream/     disposable clones of other projects (gitignored)
```

---

## Phase 0 - Bootstrap (hours)

**Do**
1. `source scripts/dev_env.sh`; verify clang, ninja, cmake, python modules
   (TOOLCHAIN_SETUP, `/local-environment`). If `sdk/thirdparty/` holds only `CMakeLists.txt`
   (kit cloned from GitHub): `bash scripts/restore_sdk_thirdparty.sh`.
2. Inventory `game/`: default.xex, extra .xex/.dll modules, `.xexp` title update, sizes,
   SHA-256 of every executable (never modify the originals). Title ID from the XEX header or
   later from the log ("Initializing shader storage for title").
3. Fill `kit.env`: GAME_NAME (lowercase, no spaces), TITLE_ID, GUEST_WIDTH/HEIGHT.
4. Build the CLI: `cd sdk && cmake --preset win-amd64 && cmake --build out/build/win-amd64
   --config Release --target rexglue` -> `sdk/out/win-amd64/Release/rexglue.exe` (~5 min).
5. `mkdir port && mv game port/game` (recreate `game/README.md`), then from `port/`:
   `../sdk/out/win-amd64/Release/rexglue.exe init --project-name $GAME_NAME --xex-path
   game/default.xex --game-root game --project-root . [--scan-dll]`, then `mkdir -p logs`
   (`port/logs/` holds images, disassembly and codegen logs). `--scan-dll` adds `.dll`
   modules only; add any other module (e.g. an extra `.xex`) with
   `rexglue.exe init module --project-root . --xex-path game/<m>.xex --guest-path "game:\<m>.xex"`.
6. From the kit root: `cp docs/templates/* docs/ && mkdir -p port/docs && mv docs/error_log.md
   port/docs/` (the error log belongs to the port).
   `git init` only if the kit is not already a repository; commit (game data is gitignored).
7. Decoded image + disassembly: `python scripts/port/xex_decode.py port/game/default.xex
   port/logs/default_image.bin` (basic compression only; otherwise `--dump_xex_image` in
   phase 2), then the disassembly `port/logs/default_full.dis` (command in
   GAME_ADAPTATION_GUIDE §1; `tools/re/*.py` read that file).

**Decide**
- Title update present -> choose base or TU **now** (switching later = new codegen). TU: put
  `default.xexp` next to `default.xex` in `port/game/`; the SDK applies it for codegen and
  runtime (log: "Loading XEX patch from"). Base revision: the TU may contain the developer's
  own fixes for timing bugs worth porting (ANY_GAME_CHECKLIST §1).
- Extra modules -> `--scan-dll` (.dll) / `init module` (other .xex); each module is its own
  codegen target.
- ISO/GOD dump -> extract to a folder first.

**Exit**: kit.env filled; project initialized; state docs exist with the "Game facts" table
started (revision hashes, TU, modules); git checkpoint.
**Detail**: ANY_GAME_CHECKLIST §1, `/bootstrap-port`.

## Phase 1 - Codegen to zero errors (hours to 1 day)

**Do**
1. `../sdk/out/win-amd64/Release/rexglue.exe codegen <game>_manifest.toml --log_file
   logs/codegen_N.txt` (never `--force`).
2. Classify each UnresolvedCall with the disassembly: import / undiscovered code / tail call
   into another function / data / jump table. Fix in the manifest with an evidence comment:
   `[entrypoint.functions.0xADDR] end = ...` (end from padding / next function), `parent =`
   chunks (a shared epilogue needs its own chunk), `switch_tables` for jump tables.
3. `python ../scripts/port/find_cross_file_gotos.py generated/default` before building
   (fall-through pairs -> extend the first function's `end`).
4. setjmp/longjmp (correctness): with the first `<game>_register.cpp` and the decoded image
   run `python ../tools/re/xdk_sigs.py match logs/default_image.bin
   generated/default/<game>_register.cpp -o ../artifacts/xdk_match.tsv`; many
   `fuzzy`/`missing` = late XDK -> `python ../tools/re/xdk_layout.py
   generated/default/<game>_register.cpp --image logs/default_image.bin
   --lib LIBCMT,xapilibi,d3d9i -o ../artifacts/xdk_layout.tsv`. Confirm `setjmp`, `longjmp`,
   `__savegprlr`/`__restgprlr` in the disassembly, set `setjmp_address` /
   `longjmp_address`, regenerate. Keep the `[rexcrt]` candidates (memcpy, memset, str*,
   Rtl*Heap...) for phase 3.

**Decide**: one root-cause class per codegen run; diff the error set after each.
**Never**: batch-guess function boundaries; `--force`.
**Exit**: 0 errors without `--force`; every override commented with evidence;
setjmp/longjmp set or recorded absent.
**Detail**: REXGLUE_PORTING_RULES (its Phases 3-10), LESSONS B, `/codegen-triage`.

## Phase 2 - Boot on the legacy Xenos path (1-2 days)

**Do**
1. CMakeLists like `reference/conan/port/CMakeLists.txt`: `rexglue_setup_target(<game>
   GPU_PLUGINS xenos)`, link `imgui::imgui d3d12 dxgi dbghelp`, xxHash include. Presets and
   flags: TOOLCHAIN_SETUP (configure `nr` RelWithDebInfo and `nr-rel` Release).
2. App class like `conan_app.h`: OnConfigurePaths (data discovery, Release logging off),
   OnPreSetup (`gpu_plugin` default xenos), GetWindowTitle.
3. Build with `bench/build.sh` (syncs SDK DLLs); run with `--mnk_mode`; logs in
   `port/out/build/nr/logs/`. No decoded image yet ->
   `--dump_xex_image=../../../logs/default_image.bin`.
4. Crash loop, one fix per run, each recorded in `port/docs/error_log.md`.
5. Scripted scenarios `bench/scenario_*.txt` (syntax: `reference/conan/bench/*.txt`) and a
   bench save created by playing into `bench/userdata_template` (never the user's saves).

**Decide**
- `[FATAL] Call to invalid or unregistered function` -> verify code in the disassembly ->
  manifest entry (register a visible thunk family at once);
  `find_unregistered_after_bctr.py` lists candidates, fix reactively.
- Guest access violation -> symbolize (crash-diagnostics patch if needed) -> missing asset?
  supply the real one : midasm guard jumping to the function's **own** bail-out (verify it
  is an exit, not a loop head).
- Timing-dependent crash/glitch -> rerun with `--ignore_thread_priorities=false
  --ignore_thread_affinities=false` (ReXGlue ignores both by default); if it disappears it
  is a scheduling race: fix the submit order at its source.
- Hang -> hang watchdog / ProcDump + windbg-tool host stacks; "sometimes" -> measure the
  rate with `bench/repro_freeze.sh` before and after the fix.
- SDK native crash -> real fix in `sdk/`, documented in KIT_SDK_CHANGES.

**Exit**: boots, menus, new game, gameplay, save/load and FMV work; scenarios reach
gameplay reproducibly.
**Detail**: LESSONS C-D, `/boot-and-crash-triage`.

## Phase 3 - Baseline and codegen optimization window (hours to 1 day)

**Do**
1. Per scenario, 3 Release runs: frame time distribution, per-thread CPU, GPU time,
   draws/resolves/passes (`--d3d_capture_out`), guest ceiling (`--gpu_null_draws=true`),
   CPU at a frame cap. Record BENCHMARKS.csv and the first EXP entries.
2. Classify the bound (PERFORMANCE_GUIDE §"Where is the frame bound?").
3. **Codegen window** - regenerating the recompiled code is cheapest now: the legacy
   baseline measures it and every later native A/B includes it. If guest-bound (usual), one
   group per codegen, paired runs:
   1. G14 `[rexcrt]` memory/string group;
   2. G1 register-locality flags, one at a time (`skip_lr` last, only if no hook reads LR);
   3. G14 heap group (all four Rtl*Heap + `rexcrt_heap_enable`).

**Decide**: keep a group only if paired runs show a gain **and** menus, gameplay, save/load,
FMV and a repro_freeze run stay clean; otherwise revert it and record why.
**Exit**: baseline and bound recorded; codegen window done or skipped with the reason.
**Detail**: PERFORMANCE_GUIDE, `/baseline-profile`.

## Phase 4 - Renderer archaeology and capture (1-3 days)

**Do**
1. XDK D3D functions (`/xdk-hook-discovery`): xdk_sigs.py / xdk_layout.py, then confirm
   every hook target (callers, arguments, PM4 constants, device offsets).
2. Game layer: device pointer global, front buffer, present function, pass table or
   markers, scene/output resolution, shadow passes, camera constants, busy-wait loops
   (GAME_ADAPTATION_GUIDE §2 lists each fact and how to find it).
3. Capture hooks that call the originals: per-frame draws/resolves/passes catalog.
4. RENDERER_ANALYSIS.md: addresses, evidence, confidence, hook value.

**Decide** the renderer strategy and record why (ANY_GAME_CHECKLIST §3): XDK hooks
(default: generic, frame-exact) or engine-level capture (only if the XDK path cannot reach
parity).
**Exit**: every hook role has a confirmed address.
**Detail**: GAME_ADAPTATION_GUIDE, XDK_D3D_NOTES, `/renderer-archaeology`.

## Phase 5 - Shader corpus (hours)

**Do**: `bash tools/shaders/build_corpus.sh` (DXC, pinned XenosRecomp + kit patch,
extraction, catalog, DXIL); wire the DXIL pack into CMake (RCDATA 2).
**Decide**
- Few containers found (compressed packages, e.g. UE3) -> add a cvar-gated container dump
  to the CreateShader hooks and harvest over the scenarios, or decompress the packages.
- Translator failure -> fix in `tools/xenosrecomp/src`, regenerate the patch
  (`git diff > ../patches/0001-conan-recomp.patch`).
- Conan-specific heuristics (shadow atlas sampler/literals) -> re-key by this game's
  reflection names or disable.
**Exit**: ~100% translate + compile; SHADER_CATALOG.md generated.
**Detail**: SHADER_PIPELINE, `/shader-pipeline`.

## Phase 6 - Native renderer bring-up (1-2 weeks)

**Do**
1. Copy `reference/conan/port/src/native/`; change only the game-specific facts
   (GAME_ADAPTATION_GUIDE §2) and names.
2. Run in `native_ab_mode` next to Xenos. Order: HUD/2D (BeginVertices, simple states) ->
   post chain -> scene passes ->
   shadows -> MSAA/alpha-to-coverage -> fidelity (pixel centers, PWL gamma, rect lists,
   copy swap, NaN scrub).
3. After every step `bench/ab_multi.sh` on several scenarios; EXP entry with PSNR.
4. Cheap D3D12 hardening (NATIVE_RENDERER_ARCHITECTURE §8): DRED, PIX pass markers,
   NOT_ZEROED, root signature 1.1, clear-rect union, batched barriers.

**Decide** by PSNR: 10-20 dB = missing/extra draws; 30-35 dB = sub-pixel shift / MSAA /
gamma class; < 45 dB otherwise = semantic bug. Isolate with `native_skip_ps`,
`native_pass_mask`, traces and dumps (VALIDATION_GUIDE). New format/primitive/packet -> log
once, implement.
**Never**: reorder, merge, cull or drop submitted draws; placeholder PSOs; "nearest" states
(NATIVE_RENDERER_ARCHITECTURE §7).
**Exit**: 45-58 dB on every scenario; 0 hangs.
**Detail**: NATIVE_RENDERER_ARCHITECTURE, VALIDATION_GUIDE, `/native-renderer`.

## Phase 7 - Fully native (days)

**Do**: NativeGraphicsSystem (sync-only PM4 consumer, fences, interrupts, vblank, gamma
ramp); hang watchdog on; sweep every scenario with `bench/repro_freeze.sh`; make native the
default; keep `native_ab_mode` for regressions.
**Decide**: guest spins in BlockUntilIdle / waits forever -> fence write-back byte order,
vblank rate (a game presenting every 2nd vblank needs vblank = 2 x fps).
**Exit**: no Xenos plugin loaded; 0 hangs.
**Detail**: NATIVE_RENDERER_ARCHITECTURE §4, `/native-graphics-system`.

## Phase 8 - Performance (days)

**Do**
1. Re-classify the bound on the native build.
2. PERFORMANCE_GUIDE checklist items not yet done (worker thread, caching, busy-waits, PSO
   precompile + embedded base, x86-64-v3, memoized hot pure guest functions, debug out of
   hot paths).
3. Gated optimizations whose gate is met, in the order of the bound table (G14/G1 if the
   codegen window skipped them; renderer/GPU-side items only if the bound says so).
4. CPU at a frame cap; handheld proxy run (affinity 0x3F); RAM/VRAM against 6 GB.

**Decide**: keep a change only if the whole frame improves in paired runs and A/B at
scale 1 is unchanged.
**Exit**: frame near the guest ceiling; no busy-waits at a cap; 0 PSOs compiled in play on
the scenario set; numbers in BENCHMARKS.csv.
**Detail**: PERFORMANCE_GUIDE, `/performance-pass`.

## Phase 9 - Options, enhancements, launcher (days)

**Do**: cfg + Win32 launcher (RELEASE_AND_SETTINGS); render resolution (arbitrary), shadow
quality, MSAA, anisotropy, FPS cap, VSync; fixes needed above the original resolution
always on (full-resolution scene, smooth effects, upscaled-resolve sampling, shadow kernel
scaling); optional enhancements (foliage A2C, FXAA, SSAO, bloom quality, dithering, shadow
smoothing, soft particles); game-behaviour options (high fps, ultrawide, FOV, mouse look)
per ANY_GAME_CHECKLIST §4.
**Decide**: an option that does not fully work is removed, not shipped; dependent options
grey out and are forced off in Apply.
**Exit**: every option tested at scale 1 (A/B unchanged) and visually at 1440p/4K; launcher
screenshots EN/ES.
**Detail**: RELEASE_AND_SETTINGS, `/settings-launcher`.

## Phase 10 - Release

**Do**: pipeline base from all scenarios and save packs (merge, never overwrite;
`tools/make_pipeline_base.sh`); `bash tools/make_release.sh [name]`; smoke test from the
release folder; long session; final PROJECT_STATE; git tag.
**Exit**: every release gate passes (VALIDATION_GUIDE §"Release gates").
**Detail**: RELEASE_AND_SETTINGS §Release, `/release`.
