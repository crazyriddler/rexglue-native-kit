---
name: local-environment
description: Verify or repair the local toolchain (clang, xwin, ninja, cmake, python modules, dxc, debuggers).
---
# Local environment

docs/TOOLCHAIN_SETUP.md lists every tool, its location on this PC and how it was obtained
without admin rights. Verify with scripts/dev_env.sh; fix kit.env paths; install only what the
current task needs; never change system settings.
