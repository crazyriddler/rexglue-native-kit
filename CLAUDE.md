# ReXGlue Native Port Kit - Autonomous Engineering Mission

## Mission
Turn the Xbox 360 game placed in `game/` (default.xex + its data, nothing recompiled yet)
into a **native PC port with a native D3D12 renderer**: statically recompiled with our
ReXGlue v0.10.0 fork (`sdk/`), no Xenos/Xenia GPU emulation in the shipped build, a
settings launcher, optimized performance, and a clean portable release.

This kit is the distilled result of a complete port (Conan, 2007): the same SDK fork,
tools, knowledge base and a full reference implementation. Reuse it; do not rediscover it.

Target architecture (achieved on Conan):
```text
recompiled game + statically linked XDK D3D (untouched)
  -> hooks on XDK D3D entry points (draws, clear, resolve, tiling, swap, shaders, constants)
  -> native renderer (capture on guest threads, record on a worker, own D3D12 queue)
  -> NativeGraphicsSystem (guest GPU sync contract, presenter, no Xenos plugin)
```

## Read first (in this order)
1. `docs/NATIVE_PORT_PLAYBOOK.md` - phases, exit criteria, exact commands.
2. `docs/LESSONS_LEARNED.md` - symptom -> cause -> fix for every problem already solved.
3. `docs/GAME_ADAPTATION_GUIDE.md` - what is game-specific in the reference renderer and
   how to find it (incl. `tools/re/xdk_sigs.py`).
4. As needed: `docs/NATIVE_RENDERER_ARCHITECTURE.md`, `docs/XDK_D3D_NOTES.md`,
   `docs/SHADER_PIPELINE.md`, `docs/PERFORMANCE_GUIDE.md`, `docs/VALIDATION_GUIDE.md`,
   `docs/RELEASE_AND_SETTINGS.md`, `docs/TOOLCHAIN_SETUP.md`, `docs/TOOLS_REFERENCE.md`,
   `docs/REXGLUE_PORTING_RULES.md` (codegen/manifest/hook rules for ReXGlue 0.10.0),
   `docs/UPSTREAM_RESEARCH.md`, `docs/STRATEGY_REVIEW.md` (verdict on a "GPU-driven
   second-generation" renderer proposal: classify the bound before optimizing),
   `docs/ANY_GAME_CHECKLIST.md` (what differs between titles: title updates, modules,
   renderer strategy, high fps, ultrawide, input; answers from UnleashedRecomp, skate3recomp
   and The Darkness Recomp).
5. The worked example: `reference/conan/` (port sources, manifest with commented
   overrides, error_log.md E001-E051, EXPERIMENT_LOG EXP-001-048, RENDERER_ANALYSIS).

## Session start
1. Read this file, then `docs/PROJECT_STATE.md`, the latest `docs/EXPERIMENT_LOG.md`
   entries and `docs/OPEN_QUESTIONS.md` (create them from `docs/templates/` in phase 0).
2. `git status` / recent commits of the kit root (initialize local git in phase 0).
3. Resume the highest-value unfinished action. Do not ask what to do next.

## Non-negotiable engineering behavior
1. Evidence before claims and fixes: disassembly, generated code, runtime values, logs.
   Label hypotheses. One root-cause class per iteration.
2. Never patch `generated/` by hand. Order: manifest -> project hook (src/) ->
   deterministic post-codegen patch -> SDK change (documented, reproducible).
3. Never hide errors (no fake no-op stubs, no `--force` as a fix). Midasm guards jump
   only to the function's own existing bail-out; verify it is an exit, not a loop head.
4. Baseline before optimizing; measure frame time (not only FPS), per-thread CPU, GPU.
   Paired runs for small differences. Record numbers in `docs/BENCHMARKS.csv`.
5. Validate every renderer change frame-exactly against Xenos (A/B by guest swap).
6. Preserve evidence: every experiment in `docs/EXPERIMENT_LOG.md`, every port error in
   `port/docs/error_log.md`, state in `docs/PROJECT_STATE.md` (fresh-session ready).
7. Prefer game-specific semantic translation over general Xenos emulation, but preserve
   game-visible semantics (pixel centers, gamma, EDRAM aliasing, NaN behavior...).
8. No scene-specific cheats. Heuristics keyed by semantic names (shader reflection),
   never by lists of shader hashes.
9. Keep diagnostics cvar-gated; no clock queries/logs/scans in per-draw paths.
10. Build and run after material changes; fix failures yourself and continue.
11. Local git commits at each milestone (no remote needed).
12. Modify the SDK fork freely when it blocks progress; document the change in
    `sdk/KIT_SDK_CHANGES.md`.

## Safety and environment constraints (this PC and this user)
- The user may be playing a release build at the same time: never kill processes by
  image name, never capture/close "the first window" of a class. Scripts only touch
  processes started from their own build dir.
- Never touch the user's saves (Documents\<game>) or release folders they use without
  asking. Benchmark saves live in `bench/userdata_template`.
- No system setting changes (developer mode, PATH, registry). No admin rights exist.
- Keep the original XEX immutable (copy before any patching).
- Game data is the user's lawful copy: never upload or distribute it.
- Downloads: GitHub/upstream sources and tools as needed for the engineering work;
  mention large ones (model weights, SDKs) to the user.

## User preferences
- Communicate with the user in **Spanish**, short status notes; work autonomously.
- Defaults = the original game. No FSR/CAS, no bicubic image upscaling (native or
  bilinear). Fixes needed at higher resolutions are always on, not options.
- Every exposed option must be tested and work; dependent options grey out.
- Launcher follows the Windows language (EN/ES/FR/DE/IT), fits 1280x800 at 150%.
- No SDK overlays, achievements toasts or debug hotkeys in the release. Window title and
  exe description = the game's name.
- Clean portable release folder (exe, needed DLLs, cfg, data/). Target handheld PCs too
  (6 GB RAM / 6 GB VRAM).
- Skip long benchmark campaigns unless relevant to the question at hand.

## Kit layout
`kit.env` (config read by every script) · `sdk/` (ReXGlue fork) · `game/` (drop zone) ·
`port/` (created by `rexglue init`) · `bench/` · `tools/` · `scripts/` · `reference/conan/` ·
`docs/` (+ `templates/`) · `artifacts/` · `release/` · `_research/upstream/` ·
`.claude/` (agents + skills).

## Skills (`.claude/skills/`) - recommended order
`/bootstrap-port` -> `/codegen-triage` -> `/boot-and-crash-triage` -> `/baseline-profile`
-> `/renderer-archaeology` (+ `/xdk-hook-discovery`) -> `/gpu-capture` -> `/shader-pipeline`
-> `/native-renderer` -> `/native-graphics-system` -> `/visual-validation` ->
`/performance-pass` -> `/settings-launcher` -> `/release` ; `/autonomous-loop` whenever the
next step is not explicit. Helpers: `/local-environment` (toolchain check/repair),
`/xdk-hook-discovery` tools also serve phase 1 (setjmp/longjmp, `[rexcrt]` candidates),
`/upstream-research` for a concrete question other ports may have answered. The codegen
optimization window sits at the end of `/baseline-profile` (playbook phase 3). Revisit
phases when evidence requires it.

## Subagents
Use the project agents for independent workstreams when the user asks for parallel work or
when a long investigation benefits from isolation: renderer-archaeologist, shader-specialist,
gpu-architect, performance-engineer, build-debugger, visual-validator, upstream-researcher.
They must write durable findings to the docs, not only return summaries.

## Completion criteria
Reproducible build; zero known unresolved calls on exercised paths; boots, menus, gameplay,
save/load, FMV, level transitions work; native renderer default with no Xenos plugin; A/B
45-58 dB on the scenario set; offline shaders + PSO precompile (no shader stutter);
measured speedup vs legacy; CPU scales with the frame cap (no busy-waits); launcher with
working options; release folder smoke-tested; docs current.
