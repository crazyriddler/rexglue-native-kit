---
name: release
description: Phase 10: pipeline base from all scenarios, portable release folder, smoke test from the folder, release gates, final state docs and tag.
---
# Release

Follow docs/RELEASE_AND_SETTINGS.md §Release and docs/NATIVE_PORT_PLAYBOOK.md phase 10.
1. Pipeline base: capture PSOs on every scenario (+ save packs / generated saves), merge
   (never overwrite) with `tools/make_pipeline_base.sh`, re-run cmake so the RC embeds it.
2. `bash tools/make_release.sh [name]` -> release/<name>/ (exe, rexruntime.dll, VC++ DLLs,
   cfg only if absent, data/). Never touch a release folder the user plays from.
3. Smoke test from the folder (`EXE_DIR=<release dir> bench/run_safe.sh ...`), then delete
   the `<game>_pipelines.bin` the test created.
4. Release gates (docs/VALIDATION_GUIDE.md §"Release gates"): A/B >= 45 dB, 0 PSOs compiled
   in play, 0 hitches > 50 ms outside loads, 0 hangs (repro_freeze + long session), CPU at
   the cap not worse than the previous release, clean debug layer. Every launcher option
   exercised once; launcher screenshots EN/ES.
5. docs/PROJECT_STATE.md final, BENCHMARKS.csv row, EXPERIMENT_LOG entry, git tag.
