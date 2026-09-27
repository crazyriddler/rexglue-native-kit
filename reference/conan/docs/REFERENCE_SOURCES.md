# Reference Projects and Research Targets

Claude should verify current upstream code before copying architecture.

## ReXGlue
https://github.com/rexglue/rexglue-sdk

Research targets:
- `IGraphicsSystem`
- RuntimeConfig graphics injection
- `rexgpu-xenos` plugin loading
- D3D12/Vulkan abstractions
- perf counters and Tracy integration
- function overrides and Mid-ASM hooks
- generated code structure

## XenosRecomp
https://github.com/hedge-dev/XenosRecomp

Research targets:
- shader container parsing
- Xenos shader -> HLSL conversion
- DXIL/SPIR-V compilation workflow
- game-specific macros/patches
- unsupported instructions/features

## Unleashed Recompiled
https://github.com/hedge-dev/UnleashedRecomp

Research targets:
- high-performance game-specific renderer
- direct translation of game draw calls instead of full GPU emulation
- multithreaded rendering
- bindless textures
- shader specialization
- removal/detection of unnecessary copies
- streaming and parallel transfer queues
- renderer hooks and recovered engine semantics

## re:Blue
https://github.com/zolaware/reblue

Research targets:
- ReXGlue integration with a custom renderer
- modern RHI design
- D3D12/Vulkan/Metal-oriented architecture
- game-specific XenosRecomp fork

Related shader fork:
https://github.com/zolaware/reblue-XenosRecomp

## NX1recomp
https://github.com/goshavindtburg/NX1recomp

Research targets:
- practical ReXGlue + XenosRecomp project structure
- shader dump/decompile tooling
- custom SDK/XenosRecomp forks
- alternate graphics backend experiments

## Xenia
Use Xenia source when necessary to understand Xenos semantics, packet formats, EDRAM/resolves, texture formats, guest GPU registers and behavior. Treat it as a semantic reference, not necessarily the desired final architecture.
