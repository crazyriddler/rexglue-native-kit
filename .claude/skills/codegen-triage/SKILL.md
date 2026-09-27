---
name: codegen-triage
description: Phase 1: run rexglue codegen and drive analysis errors to zero with evidence-based manifest overrides.
---
# Codegen triage

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 1 - read it and follow it.
Classification rules per error type: docs/REXGLUE_PORTING_RULES.md (its Phase 5) and
LESSONS_LEARNED B. Model of evidence comments: reference/conan/port/conan_manifest.toml.
Every fix -> an E-entry in port/docs/error_log.md; the setjmp/longjmp result and the [rexcrt]
candidates -> PROJECT_STATE "Game facts".
