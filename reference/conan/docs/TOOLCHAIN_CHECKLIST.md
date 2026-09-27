# Toolchain checklist

This is a discovery checklist, not a fixed dependency list. Use only what the actual port/backend requires.

## Core
- Claude Code CLI in PATH
- compiler/build system used by the port
- CMake if the project uses CMake
- Ninja and/or MSBuild as required
- Visual Studio / Visual Studio Build Tools for MSVC projects
- Windows SDK matching the project requirements
- Git: optional for the port itself, useful for local checkpoints and public reference clones

## Graphics / shaders
- DirectX Shader Compiler (`dxc`) when using HLSL/DXIL or SPIR-V via DXC
- Vulkan SDK only if Vulkan tooling/build requires it
- XenosRecomp or a project-specific fork when needed

## Profiling / capture
Use whichever is compatible with the active API/GPU:
- RenderDoc
- PIX on Windows
- PresentMon
- Tracy
- NVIDIA Nsight Graphics
- Radeon GPU Profiler / Radeon GPU Detective
- GPUView/WPA when system-level scheduling evidence is needed

## Validation
- deterministic screenshot capture if practical
- image comparison tooling/scripts
- structured benchmark logs

## Rule
Do not spend time installing every tool on this list. First identify the current bottleneck/question, then obtain the tool that answers it.
