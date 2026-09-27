---
name: shader-pipeline
description: Phase 5: offline shader corpus with the patched XenosRecomp (extract, hash, HLSL, DXIL, reflection, embed).
---
# Shader pipeline

`bash tools/shaders/build_corpus.sh`; inspect artifacts/shaders/catalog.json and
docs/SHADER_CATALOG.md; fix translator failures in tools/xenosrecomp/src and regenerate the
patch; review the Conan-specific heuristics (shadow atlas, camera constant name in
gen_projection_regs.py) against this game's reflection (docs/SHADER_PIPELINE.md). Embed the
DXIL pack (RCDATA 2) through CMake.
