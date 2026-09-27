---
name: baseline-profile
description: Phase 3 and later: quantitative baselines (frame time distribution, per-thread CPU, GPU time, draws/resolves, null-draw ceiling).
---
# Baseline profile

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 3 (baseline, bound classification, codegen
optimization window). Measurement method and tools: docs/PERFORMANCE_GUIDE.md "Measure the
right thing" (window t=50..90 s, `bench/run_safe.sh`, `tools/bench_summary.py`,
`tools/thread_cpu.py`, `--gpu_null_draws=true`). Numbers -> BENCHMARKS.csv (incl.
guest_ceiling_ms); ranked bottlenecks -> OPEN_QUESTIONS; each codegen group -> an EXP entry.
