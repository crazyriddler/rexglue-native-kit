---
name: local-environment
description: Verify or repair the local toolchain (clang, xwin, ninja, cmake, python modules, dxc, debuggers).
---
# Local environment

docs/TOOLCHAIN_SETUP.md lists every tool and its kit-local location. Install or repair:
`bash scripts/setup_toolchain.sh --accept-microsoft-license` (idempotent; delete a component's
folder under tools/toolchain/ to reinstall it). Verify with `source scripts/dev_env.sh`. Never
install system-wide or change system settings; a failing step is fixed in the script.
- `sdk/thirdparty/` is in git; a missing or empty entry: `bash scripts/restore_sdk_thirdparty.sh <name>`.
- Kit self-test: `python -m pytest scripts/tests` (pytest + a C++ compiler).
