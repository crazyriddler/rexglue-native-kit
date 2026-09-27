---
name: performance-pass
description: Phase 8: evidence-driven optimization of the native port (unlocked frame time and CPU/power at a frame cap).
---
# Performance pass

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 8. Decision source: docs/PERFORMANCE_GUIDE.md
("Where is the frame bound?" -> checklist -> gated optimizations G1-G14 whose gate is met).
One change per measurement, paired runs, whole-frame gain required, A/B at scale 1
unchanged; include CPU at a frame cap and a handheld affinity proxy run. Every attempt,
kept or reverted -> EXP entry + BENCHMARKS.csv.
