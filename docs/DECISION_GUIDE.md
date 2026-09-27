# Decision guide: how to choose the next step

Start here at every session and whenever the next step is not obvious. This file holds only
cross-phase guidance; each piece of information has one home, linked from here:

| What | Its only home |
|---|---|
| Engineering rules, safety, user preferences, completion criteria | CLAUDE.md |
| What to do in each phase (commands, decisions, exit criteria) | NATIVE_PORT_PLAYBOOK.md |
| Where the frame is bound and which optimization to try (G1-G14) | PERFORMANCE_GUIDE.md |
| Known problems and their fixes | LESSONS_LEARNED.md |
| Current state of this port | docs/PROJECT_STATE.md |

## 1. Where am I?

1. `docs/PROJECT_STATE.md` -> current phase and "Next actions" (no file: phase 0).
2. Latest entries of `docs/EXPERIMENT_LOG.md`, `docs/OPEN_QUESTIONS.md`,
   `port/docs/error_log.md`, `git log`.
3. Open that phase in the playbook; continue with the first unmet **Do** item or **Exit**
   criterion.

## 2. The work loop

```text
pick the highest-value open item (§3) -> known answer? (LESSONS_LEARNED, §4)
  -> falsifiable hypothesis -> evidence (disassembly / generated code / logs / trace / profile)
  -> ONE change of ONE root-cause class -> bench/build.sh -> run the scenario
  -> measure / A/B -> keep or revert -> record (§5) -> git commit -> next
```

A failed experiment is a result: record it and choose the next hypothesis. Decide between
engineering options with evidence instead of asking the user; stop only for a real external
blocker (e.g. a missing game file) or at a milestone with next actions recorded.

## 3. Priorities when several things compete

1. It builds and runs (toolchain, codegen, crash, hang).
2. The game behaves correctly (logic, saves, FMV, audio, no races).
3. The native image matches Xenos frame-exactly (A/B 45-58 dB).
4. Coverage: more content reached and validated (levels, menus, cinematics, transitions).
5. Performance, only on the measured bottleneck (PERFORMANCE_GUIDE bound table).
6. Options, launcher, enhancements (defaults = the original game).
7. Tooling and generalization for the next game.

A lower item never costs a higher one: no speedup that loses A/B dB, no option that adds a
hang, no enhancement enabled by default.

## 4. Symptom router

| Symptom | First look |
|---|---|
| Toolchain, CMake, missing third-party code | LESSONS A, TOOLCHAIN_SETUP |
| UnresolvedCall, `undeclared label`, codegen oddities | LESSONS B, REXGLUE_PORTING_RULES |
| Crash, unregistered function, guest AV, hang, "sometimes" | LESSONS C, playbook phase 2 **Decide** |
| Legacy (Xenos) path wrong or slow | LESSONS D |
| Native image wrong (black, colors, missing geometry, shifted, stale) | LESSONS E, VALIDATION_GUIDE "Diagnosing a difference" |
| Game waits forever with NativeGraphicsSystem | LESSONS E (fence byte order), playbook phase 7 |
| Low fps, stutter, high CPU at a cap, memory | PERFORMANCE_GUIDE bound table, LESSONS F |
| Launcher, cfg, release | LESSONS G, RELEASE_AND_SETTINGS |
| Something Conan never had (TU, modules, late XDK, UE3 shaders, 60 fps, ultrawide, races) | ANY_GAME_CHECKLIST |
| "Redesign the renderer (GPU-driven, ExecuteIndirect, culling)?" | STRATEGY_REVIEW (usually no; rules in NATIVE_RENDERER_ARCHITECTURE §7) |
| Did another project solve this? | UPSTREAM_RESEARCH |

## 5. What to record, where

| What | Where |
|---|---|
| Phase, status, next actions (a fresh session must be able to continue) | `docs/PROJECT_STATE.md` |
| Game facts (revision, TU, XDK, setjmp, rexcrt, flags, strategy) | PROJECT_STATE "Game facts" table |
| Every experiment, positive or negative, with numbers | `docs/EXPERIMENT_LOG.md` (`scripts/new_experiment.ps1`) |
| Every port error: symptom -> evidence -> cause -> fix | `port/docs/error_log.md` |
| Hypotheses with confidence | `docs/OPEN_QUESTIONS.md` |
| Benchmark numbers | `docs/BENCHMARKS.csv` |
| Addresses, hooks, passes, renderer strategy | `docs/RENDERER_ANALYSIS.md` |
| SDK changes | `sdk/KIT_SDK_CHANGES.md` (+ `sdk/patches/thirdparty/` for third-party code) |
| A lesson true for any game | kit `docs/LESSONS_LEARNED.md` or `docs/ANY_GAME_CHECKLIST.md` |

## 6. Document map

| Need | Document |
|---|---|
| Procedure per phase | NATIVE_PORT_PLAYBOOK.md |
| Known problems and fixes (symptom -> cause -> fix, with E/EXP references) | LESSONS_LEARNED.md |
| What differs in a new game, answers from other ports | ANY_GAME_CHECKLIST.md |
| Game-specific facts of the reference renderer and how to find them | GAME_ADAPTATION_GUIDE.md |
| How the native renderer works, its invariants and hardening | NATIVE_RENDERER_ARCHITECTURE.md |
| XDK D3D device, resources, command stream, Xenos semantics | XDK_D3D_NOTES.md |
| Shader corpus and the XenosRecomp patch | SHADER_PIPELINE.md |
| Measuring, bound classification, checklist, gated optimizations | PERFORMANCE_GUIDE.md |
| A/B, diagnosis, extra validation layers, release gates | VALIDATION_GUIDE.md |
| cfg, launcher, app hooks, release folder | RELEASE_AND_SETTINGS.md |
| Toolchain install (kit-local), SDK third-party code, configure/build | TOOLCHAIN_SETUP.md |
| Every tool and script, SDK cvars | TOOLS_REFERENCE.md |
| Recompiler, manifest and hook rules (ReXGlue 0.10.0) | REXGLUE_PORTING_RULES.md |
| Why some popular optimizations are rejected; proven vs untested | STRATEGY_REVIEW.md |
| Other projects: index, evidence and file references | UPSTREAM_RESEARCH.md |
| Local workspace contract (no remote required) | LOCAL_WORKSPACE.md |
| Per-game state files to copy in phase 0 | templates/ |
| The complete worked example | reference/conan/ |
