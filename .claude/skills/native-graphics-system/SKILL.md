---
name: native-graphics-system
description: Phase 7: remove the Xenos plugin - NativeGraphicsSystem provides the presenter and the guest GPU sync contract.
---
# Native graphics system

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 7. What NativeGraphicsSystem must provide
(MMIO, sync-only PM4 consumer, fence byte order, WAIT_REG_MEM, interrupts, vblank, gamma):
docs/NATIVE_RENDERER_ARCHITECTURE.md §4; source: reference/conan/port/src/native/native_graphics_system.*.
Install it in OnPreSetup (config.graphics); keep native_ab_mode loading Xenos for regressions.
