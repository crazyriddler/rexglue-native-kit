---
name: gpu-capture
description: Instrument the working path to catalog the game workload: draws, resolves, passes, shaders, states, formats, primitive types, packet types.
---
# Gpu capture

Capture hooks call the originals (d3d_capture.cpp model). Deduplicate by stable IDs (shader
container hash, state keys); JSON output (`--d3d_capture_out`), summaries in
docs/PIPELINE_CATALOG.md and SHADER_CATALOG.md. Note formats, primitive types (rect/quad lists),
packet types and features Conan did not use: they are the new work for the native renderer.
