---
name: baseline-profile
description: Phase 3 and later: quantitative baselines (frame time distribution, per-thread CPU, GPU time, draws/resolves, null-draw ceiling).
---
# Baseline profile

Follow docs/PERFORMANCE_GUIDE.md. Scenario runs with bench/run_safe.sh, window t=50..90 s,
3 runs (or paired A/B runs), unlocked and at a frame cap; per-thread CPU, power, RAM/VRAM;
`--d3d_capture_out` for draws/resolves/passes; `--gpu_null_draws` ceiling on the legacy path.
End with the bound classification (docs/PERFORMANCE_GUIDE.md §"Where is the frame bound?"):
guest ceiling, worker busy, GPU time. Record BENCHMARKS.csv (incl. guest_ceiling_ms) +
EXPERIMENT_LOG; rank bottlenecks in OPEN_QUESTIONS; continue.
Then the codegen optimization window (docs/NATIVE_PORT_PLAYBOOK.md end of phase 3): G14 [rexcrt]
and G1 flags one group per codegen, paired runs, keep only clean wins.
