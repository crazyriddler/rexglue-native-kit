---
name: renderer-archaeology
description: Phase 4: recover the game renderer structure and every hook point needed by the native renderer.
---
# Renderer archaeology

1. /xdk-hook-discovery for the XDK D3D functions.
2. Game layer: device pointer global, front buffer, present function, pass table/markers, batch renderer, material bind, post effects, HUD path (docs/GAME_ADAPTATION_GUIDE.md section 2; reference/conan/docs/RENDERER_ANALYSIS.md as a model).
3. Runtime confirmation with capture hooks that call the originals.
4. docs/RENDERER_ANALYSIS.md with address, evidence, confidence, hook value.
5. Renderer strategy decision with evidence (docs/ANY_GAME_CHECKLIST.md §2): XDK hooks (default) vs engine-level capture; record it in RENDERER_ANALYSIS.md.
