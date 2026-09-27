---
name: native-graphics-system
description: Phase 7: remove the Xenos plugin - NativeGraphicsSystem provides the presenter and the guest GPU sync contract.
---
# Native graphics system

Adapt reference native_graphics_system.* (docs/NATIVE_RENDERER_ARCHITECTURE.md section 4):
MMIO, sync-only PM4 consumer (fences in host order via GpuSwap, big-endian RPtr write-back,
WAIT_REG_MEM blocking, interrupts, swap counting, DC_LUT gamma), vblank thread (check how many
vblanks the game waits per frame), BlockOnFence poll hook sleeping on the fence condition.
Install it in OnPreSetup (config.graphics). Validate: every scenario, 0 watchdog hangs, frame
dumps correct; keep native_ab_mode loading Xenos for regressions.
