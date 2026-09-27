---
name: bootstrap-port
description: Phase 0: prepare a new port in the kit (toolchain check, game inventory, kit.env, rexglue CLI, rexglue init, state docs, local git).
---
# Bootstrap port

1. Read CLAUDE.md, docs/NATIVE_PORT_PLAYBOOK.md (phase 0), docs/TOOLCHAIN_SETUP.md.
2. `source scripts/dev_env.sh`; verify clang, ninja, cmake, python modules.
3. Inventory game/: XEX(es), DLL modules, size, SHA-256; title ID.
4. Fill kit.env (GAME_NAME lowercase, TITLE_ID, GUEST_WIDTH/HEIGHT).
5. Build the CLI: `cd sdk && cmake --preset win-amd64 && cmake --build out/build/win-amd64 --config Release --target rexglue`.
6. `mkdir port && mv game port/game`; from port/: `../sdk/out/win-amd64/Release/rexglue.exe init --project-name $GAME_NAME --xex-path game/default.xex --game-root game --project-root . [--scan-dll]`.
7. Copy docs/templates/* to docs/ and error_log.md to port/docs/; git init + first commit.
8. Decoded image (scripts/port/xex_decode.py, or later --dump_xex_image) + objdump into port/logs/.
9. Continue with /codegen-triage.
