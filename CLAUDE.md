# ReXGlue Native Port Kit - Autonomous Engineering Mission

## Mission
Turn the Xbox 360 game placed in `game/` (default.xex + its data, nothing recompiled yet)
into a **native PC port with a native D3D12 renderer**: statically recompiled with our
ReXGlue v0.10.0 fork (`sdk/`), no Xenos/Xenia GPU emulation in the shipped build, a
settings launcher, optimized performance, and a clean portable release.

This kit is the distilled result of a complete port (Conan, 2007) plus a survey of other
static-recompilation ports: the same SDK fork, tools, knowledge base and a full reference
implementation (`reference/conan/`). Reuse it; do not rediscover it.

Target architecture (achieved on Conan):
```text
recompiled game + statically linked XDK D3D (untouched)
  -> hooks on XDK D3D entry points (draws, clear, resolve, tiling, swap, shaders, constants)
  -> native renderer (capture on guest threads, record on a worker, own D3D12 queue)
  -> NativeGraphicsSystem (guest GPU sync contract, presenter, no Xenos plugin)
```

## How to work (read in this order)
1. `docs/DECISION_GUIDE.md` - where you are, the work loop, priorities, symptom router,
   where to record, which document holds what. Read it at every session start.
2. `docs/NATIVE_PORT_PLAYBOOK.md` - the phase you are in: Do / Decide / Never / Exit, with
   the exact commands.
3. `docs/LESSONS_LEARNED.md` - the section of your phase before debugging anything.
4. Other documents on demand, via the document map of the decision guide.

Session start: `docs/PROJECT_STATE.md` (phase, next actions), latest
`docs/EXPERIMENT_LOG.md` entries, `docs/OPEN_QUESTIONS.md`, `port/docs/error_log.md`,
`git log`; then resume the highest-value unfinished action. Do not ask what to do next.

Toolchain: everything lives in `tools/toolchain/`, installed by
`bash scripts/setup_toolchain.sh --accept-microsoft-license` (phase 0; re-run it whenever a
tool is missing). Shell state does not persist between commands: kit scripts load the
environment themselves; prefix direct clang/cmake/ninja/python calls with
`source scripts/dev_env.sh && `. If a setup step fails, fix the script, never install
system-wide.

## Non-negotiable engineering rules
1. Evidence before claims and fixes: disassembly, generated code, runtime values, logs.
   Label hypotheses. One root-cause class per iteration.
2. Never patch `generated/` by hand. Fix order: manifest -> project hook (src/) ->
   deterministic post-codegen patch (anchor + hard failure if missing) -> SDK change
   (documented in `sdk/KIT_SDK_CHANGES.md`, reproducible).
3. Never hide errors: no fake no-op stubs, no `--force` as a fix. Midasm guards jump only to
   the function's own existing bail-out; verify it is an exit, not a loop head.
4. Baseline before optimizing; classify the bound first; measure frame time (not only FPS),
   per-thread CPU, GPU; paired runs for small differences; numbers in `docs/BENCHMARKS.csv`.
5. Validate every renderer change frame-exactly against Xenos (A/B by guest swap number).
6. The native renderer never reorders, merges, culls, drops or substitutes a draw the game
   submitted, and maps state exactly (NATIVE_RENDERER_ARCHITECTURE §7).
7. Preserve game-visible semantics (pixel centers, gamma, EDRAM aliasing, NaN behavior,
   MSAA sample count) while preferring game-specific translation over general emulation.
8. No scene-specific cheats; heuristics keyed by semantic names (shader reflection), never
   by lists of shader hashes.
9. Diagnostics cvar-gated; no clock queries, logs or scans in per-draw paths.
10. Build and run after material changes; fix failures yourself and continue.
11. Record everything (DECISION_GUIDE §5); local git commit at each milestone (no remote
    needed).
12. Modify the SDK fork freely when it blocks progress; document it in
    `sdk/KIT_SDK_CHANGES.md` and run the SDK self-tests (VALIDATION_GUIDE). Never replace
    `sdk/` with an upstream checkout.

## Safety and environment constraints
- The user may be playing a release build at the same time: never kill processes by image
  name, never capture/close "the first window" of a class. Scripts only touch processes
  started from their own build dir.
- Never touch the user's saves (Documents\<game>) or release folders they use without
  asking. Benchmark saves live in `bench/userdata_template`.
- No system setting changes (developer mode, PATH, registry). No admin rights exist.
- Keep the original XEX/.xexp immutable (copy before any patching).
- Game data is the user's lawful copy: never upload or distribute it.
- Downloads: GitHub/upstream sources and tools as needed for the engineering work; mention
  large ones (model weights, SDKs) to the user.

## User preferences (kit defaults; the only copy, other documents refer here)
- Communicate with the user **in the language they write in** (the kit author writes Spanish),
  short status notes; work autonomously. Code, docs, logs and commit messages in English.
- Defaults = the original game (resolution, shadows, the game's MSAA, frame rate, VSync on,
  fullscreen). Enhancements and game-behaviour changes (high fps, ultrawide, FOV) are
  options, off by default.
- No FSR/CAS/DLSS/TAA-style upscalers, no bicubic image upscaling (native or bilinear).
  Fixes needed at higher resolutions are always on, not options.
- Every exposed option must be tested and work; dependent options grey out.
- Launcher follows the Windows language (EN/ES/FR/DE/IT, English otherwise), fits 1280x800
  at 150%.
- No SDK overlays, achievement toasts or debug hotkeys in the release. Window title and exe
  description = the game's name.
- Clean portable release folder (exe, needed DLLs, cfg, data/); saves stay in
  Documents\<game>. Target handheld PCs too (6 GB RAM / 6 GB VRAM).
- Skip long benchmark campaigns unless relevant to the question at hand.

## Skills and agents
Each skill is the short checklist of one playbook phase:

| Phase | Skill |
|---|---|
| 0 | `/bootstrap-port` (+ `/local-environment`) |
| 1 | `/codegen-triage` |
| 2 | `/boot-and-crash-triage` |
| 3 | `/baseline-profile` (ends with the codegen optimization window) |
| 4 | `/renderer-archaeology`, `/xdk-hook-discovery`, `/gpu-capture` |
| 5 | `/shader-pipeline` |
| 6 | `/native-renderer`, `/visual-validation` |
| 7 | `/native-graphics-system` |
| 8 | `/performance-pass` |
| 9 | `/settings-launcher` |
| 10 | `/release` |
| any | `/autonomous-loop` (next step not explicit), `/upstream-research` |

Project agents (renderer-archaeologist, shader-specialist, gpu-architect,
performance-engineer, build-debugger, visual-validator, upstream-researcher) are for
independent workstreams when the user asks for parallel work or a long investigation
benefits from isolation. They write durable findings to the docs, not only summaries.

## Completion criteria
Reproducible build; zero known unresolved calls on exercised paths; boots, menus, gameplay,
save/load, FMV, level transitions work; native renderer default with no Xenos plugin; A/B
45-58 dB on the scenario set; offline shaders + PSO precompile (0 PSOs compiled in play);
measured speedup vs legacy; CPU scales with the frame cap (no busy-waits); launcher with
working options; release gates pass (VALIDATION_GUIDE); release folder smoke-tested; docs
current.
