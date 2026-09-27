---
name: native-renderer
description: Phase 6: port and adapt the reference native renderer; bring it up pass family by pass family with frame-exact A/B.
---
# Native renderer

1. Copy reference/conan/port/src/native into port/src/native; change only the game-specific facts (docs/GAME_ADAPTATION_GUIDE.md) and names.
2. Start in native_ab_mode next to Xenos. Order: HUD/2D -> post chain -> scene -> shadows -> MSAA/A2C -> fidelity items (pixel centers, PWL gamma, rect lists, copy swap, NaN scrub).
3. After each step: bench/ab_multi.sh on several scenarios; isolate differences (docs/VALIDATION_GUIDE.md); log EXP entries.
4. Implement formats/primitives/packets the new game uses that Conan did not.
5. Exit: 45-58 dB on all scenarios, 0 hangs.
