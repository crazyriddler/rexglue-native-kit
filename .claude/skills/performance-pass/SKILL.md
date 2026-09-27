---
name: performance-pass
description: Phase 8: evidence-driven optimization of the native port (unlocked frame time and CPU/power at a frame cap).
---
# Performance pass

Re-profile; pick one measured bottleneck; apply the matching item of the
docs/PERFORMANCE_GUIDE.md checklist; measure on the same scenario (paired runs); keep or revert;
log. Always check CPU at a frame cap (busy-waits) - it matters for handheld PCs.
