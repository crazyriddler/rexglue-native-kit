---
name: boot-and-crash-triage
description: Phase 2: build, boot and fix runtime crashes/hangs until menus, gameplay, save/load and FMV work on the legacy path.
---
# Boot and crash triage

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 2 - its **Decide** list classifies every
failure type (unregistered function, guest AV, timing-dependent, hang, SDK crash).
Evidence chains of solved cases: LESSONS_LEARNED C-D and reference/conan/port/docs/error_log.md
(E010-E051). One fix per run; every fix -> port/docs/error_log.md.
