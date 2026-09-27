# GPU Migration Playbook

This is a working checklist, not a rigid waterfall.

## 1. Prove where time goes
Never assume Xenos emulation is the only bottleneck. Capture CPU/GPU timing and inspect stalls. Identify whether the largest cost is command processing, guest CPU submission, state translation, EDRAM emulation, copies/resolves, shaders, synchronization, resource conversion, driver overhead, or presentation.

## 2. Recover semantic boundaries
The biggest performance opportunity usually comes from hooking above the generic GPU packet level. Search for stable game-level renderer concepts: materials, render items, passes, views, lights, scene submission, resource wrappers, and shader tables.

A useful hook has enough semantic information to issue a native draw without replaying all lower-level Xbox state mutation.

## 3. Catalog the actual workload
General Xenos supports far more combinations than a single game uses. Instrument the reference path and deduplicate:
- shader pairs
- vertex declarations
- RT/depth formats
- blend/depth/raster combinations
- texture/sampler patterns
- resolves/copies
- pass ordering

This defines the finite compatibility surface the native renderer must support.

## 4. Translate shaders offline
Dump the game's Xenos shader binaries, give them stable hashes, translate through XenosRecomp or a fork, compile to the native API, and cache results.

Patch game-specific incompatibilities in the recompiler or generated HLSL rather than adding a general emulator layer unless evidence requires it.

## 5. Build a native proof
Start with a rendering slice that exercises real bindings and output. Intercept the corresponding game renderer call, construct host resources/state, bind translated shaders, and issue native draws.

Validate against a reference screenshot/capture and profile both paths.

## 6. Expand coverage by pass/material family
Use capture frequency and frame-time contribution to pick the next migration target. Maintain native/legacy routing until coverage is sufficient.

## 7. Remove legacy-only costs
Once a semantic path is native, ensure it does not still pay for unnecessary Xenos packet creation, register tracking, EDRAM bookkeeping, format conversion, resource copies, or duplicated synchronization.

## 8. Optimize the native architecture
Only after meaningful native coverage, consider:
- bindless descriptors
- PSO precompilation/cache
- multithreaded command recording
- async transfer queues
- resource streaming staging pools
- resolve/copy elimination
- render-pass merging
- redundant state suppression
- frame overlap
- shader specialization
- material sorting/batching

## 9. Validate broadly
The reference backend is a behavioral oracle. Compare many scenes and edge cases, not only the benchmark scene.
