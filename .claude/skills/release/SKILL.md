---
name: release
description: Phase 10: pipeline base from all scenarios, portable release folder, smoke test from the folder, release gates, final state docs and tag.
---
# Release

Procedure: docs/NATIVE_PORT_PLAYBOOK.md Phase 10; commands and folder contents:
docs/RELEASE_AND_SETTINGS.md §Release; gates: docs/VALIDATION_GUIDE.md §"Release gates".
After the smoke test from the release folder, delete the `<game>_pipelines.bin` it created.
Never touch a release folder the user plays from.
