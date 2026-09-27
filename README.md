# ReXGlue Native Port Kit

A kit for turning an Xbox 360 game into a **native PC port with a native Direct3D 12
renderer**, driven autonomously by Claude Code. The game is statically recompiled with a
ReXGlue v0.10.0 fork, the XDK Direct3D calls are hooked and rendered natively (no Xenos/Xenia
GPU emulation in the shipped build), and the result is optimized, gets a settings launcher
and ships as a clean portable folder.

The kit is the distilled result of a complete port (Conan, 2007: frame-exact against the
emulated renderer at 47-58 dB PSNR, faster than the emulated path, native resolution scaling,
launcher, release) plus a survey of other static-recompilation ports (UnleashedRecomp,
reblue, skate3recomp, The Darkness Recomp, LostOdysseyRecomp, AC6_recomp and more).

> **Bring your own game.** This repository contains no game code or assets. Use files
> dumped from your own copy; never upload or share them.

## Quick start

1. Copy or clone this repository to a new folder per game (a path without spaces is best).
   Keep a pristine copy of the kit as a template.
2. Put the game in `game/`: `default.xex`, any other `.xex`/`.dll` modules and all data
   folders, as extracted from the disc (see `game/README.md` for title updates).
3. Open Claude Code in the folder and paste `START_PROMPT.md`, or run
   `START_CLAUDE_FULL_ACCESS.cmd` (starts Claude Code with that prompt and full permissions).
4. To continue another day: `RESUME_CLAUDE_FULL_ACCESS.cmd` or paste `RESUME_PROMPT.md`.

Claude Code works autonomously, records its state in `docs/PROJECT_STATE.md` and reports
short progress notes in the language you write to it.

**Toolchain: nothing to install by hand.** In phase 0 Claude Code runs
`scripts/setup_toolchain.sh`, which installs LLVM/Clang, CMake, Ninja, Python, the MSVC CRT +
Windows SDK (via xwin) and the Visual C++ runtime inside `tools/toolchain/` (~1.1 GB download,
~4 GB on disk; no admin rights, no Visual Studio, nothing outside the folder). The CRT, SDK and
runtime are Microsoft software: **starting the kit means you accept Microsoft's license terms**
for them (https://go.microsoft.com/fwlink/?LinkId=2086102). What you need beforehand: Windows
10/11 x64 and Claude Code (which already requires Git for Windows / Git Bash).

## How Claude Code navigates the kit

| Step | File | Role |
|---|---|---|
| 1 | `CLAUDE.md` | Controlling instruction: engineering rules, safety, user preferences, completion criteria, phase -> skill map |
| 2 | `docs/DECISION_GUIDE.md` | Entry point of every session: where the port is, the work loop, priorities, symptom router, what to record where, which document holds what |
| 3 | `docs/NATIVE_PORT_PLAYBOOK.md` | The procedure: phases 0-10, each with Do (exact commands) / Decide / Never / Exit |
| 4 | Other `docs/` | Read on demand via the decision guide's document map (lessons learned, performance, validation, architecture, per-game variability...) |
| - | `.claude/skills/`, `.claude/agents/` | One skill per phase (pointer to its playbook card), specialist agents for parallel work |

Phases: 0 bootstrap -> 1 codegen -> 2 boot on the emulated renderer -> 3 baseline and codegen
optimizations -> 4 renderer archaeology -> 5 offline shaders -> 6 native renderer with
frame-exact A/B -> 7 fully native (no GPU emulation) -> 8 performance -> 9 options and
launcher -> 10 release.

## Repository layout

| Path | Content |
|---|---|
| `sdk/` | ReXGlue v0.10.0 fork (base upstream `c94f5eb`) with all its third-party code vendored (a clone builds without extra downloads); changes in `sdk/KIT_SDK_CHANGES.md`, third-party patches in `sdk/patches/thirdparty/` |
| `game/` | Drop zone for the game (moved to `port/game` in phase 0) |
| `docs/` | Knowledge base and `templates/` for the per-game state files |
| `reference/conan/` | The complete worked example: port sources (native renderer, NativeGraphicsSystem, launcher), commented manifest, error log E001-E051, experiment log EXP-001-048 |
| `tools/` | Benchmark, profiling, capture and image-comparison tools; shader pipeline (patched XenosRecomp, DXC); reverse-engineering tools (`re/xdk_sigs.py`, `re/xdk_layout.py`); release builder |
| `bench/` | Scripted, reproducible runs; frame-exact A/B; freeze reproduction; optimization baselines |
| `scripts/` | Kit-local toolchain install (`setup_toolchain.sh`) and environment (`dev_env.sh`), SDK third-party re-vendoring, port helpers (XEX decoding, codegen scanners), self-tests (`python -m pytest scripts/tests`) |
| `kit.env` | Per-project configuration read by every script |

## Requirements

Windows 10/11 x64, Claude Code with Git for Windows (Git Bash), a DirectX 12 GPU to run and
validate the port, and network access in phase 0. Everything else is installed inside the kit
by `scripts/setup_toolchain.sh` (versions and details: `docs/TOOLCHAIN_SETUP.md`); optional
ProcDump/RenderDoc with `--debug-tools`.

## What is proven and what depends on the game

Proven end to end on one game (Conan, XDK 2.0.5632): every phase, the reference renderer, the
lessons learned and the tools. Every new game brings its own recompilation issues, XDK
revision and engine; the kit contains the mechanisms, the known answers
(`docs/ANY_GAME_CHECKLIST.md`) and the diagnostic method, and lists precisely which parts are
proven, proven elsewhere or still untested (`docs/STRATEGY_REVIEW.md` §6).

## Credits

ReXGlue SDK (rexglue/rexglue-sdk), XenosRecomp and UnleashedRecomp (hedge-dev), reblue and
reblue-XenosRecomp (zolaware), plume (renderbag), Xenia, and the static-recompilation projects
cited in `docs/UPSTREAM_RESEARCH.md`. The XDK symbol table in `tools/re/` comes from the CC0
dc3-decomp project.
