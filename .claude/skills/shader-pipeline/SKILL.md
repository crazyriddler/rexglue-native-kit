---
name: shader-pipeline
description: Phase 5: offline shader corpus with the patched XenosRecomp (extract, hash, HLSL, DXIL, reflection, embed).
---
# Shader pipeline

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 5; pipeline internals and the XenosRecomp
patch features: docs/SHADER_PIPELINE.md. Check artifacts/shaders/catalog.json and
docs/SHADER_CATALOG.md for failures; review the Conan-specific heuristics (shadow atlas,
camera constant name in tools/shaders/gen_projection_regs.py) against this game's reflection.
