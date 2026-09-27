---
name: native-renderer
description: Phase 6: port and adapt the reference native renderer; bring it up pass family by pass family with frame-exact A/B.
---
# Native renderer

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 6 (order of bring-up, PSNR classes, Never).
What to change in the reference sources: docs/GAME_ADAPTATION_GUIDE.md §2-3. How it works
and its invariants/hardening: docs/NATIVE_RENDERER_ARCHITECTURE.md. Diagnosis of every A/B
difference: /visual-validation. Each step -> an EXP entry with PSNR per scenario.
