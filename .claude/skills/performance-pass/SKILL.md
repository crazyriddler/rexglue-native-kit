---
name: performance-pass
description: Phase 8: evidence-driven optimization of the native port (unlocked frame time and CPU/power at a frame cap).
---
# Performance pass

Re-profile and classify the bound (docs/PERFORMANCE_GUIDE.md §"Where is the frame bound?").
Pick one measured bottleneck; apply the matching checklist item, or a gated optimization
(G1-G11) only if its gate is met; measure on the same scenario (paired runs); A/B at scale 1
unchanged; keep or revert; log. Guest-bound (the usual case): G14 native CRT, G1 codegen flags,
G2 XDK D3D cost, G11 PGO before anything renderer-side. Never reorder, drop or cull submitted draws
(NATIVE_RENDERER_ARCHITECTURE.md §7). Always check CPU at a frame cap (busy-waits) - it
matters for handheld PCs.
