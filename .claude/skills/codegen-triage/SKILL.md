---
name: codegen-triage
description: Phase 1: run rexglue codegen and drive analysis errors to zero with evidence-based manifest overrides.
---
# Codegen triage

1. `../sdk/out/win-amd64/Release/rexglue.exe codegen <game>_manifest.toml --log_file logs/codegen_N.txt` (no --force).
2. For each UnresolvedCall: disassemble around target and caller; classify import / undiscovered code / tail call into a function / data / jump table (docs/REXGLUE_PORTING_RULES.md phase 5).
3. Fix with `[entrypoint.functions.0xADDR] end=` (end read from padding/next function) or `parent=` chunks (a shared epilogue needs its own chunk). Comment each override with the evidence (see reference/conan/port/conan_manifest.toml).
4. One class per iteration; rerun codegen and diff the error set.
5. Before building: `python scripts/port/find_cross_file_gotos.py generated/default` (fall-through pairs -> override the first function's end).
6. Exit: 0 errors without --force. Log E-entries in port/docs/error_log.md.
