---
name: xdk-hook-discovery
description: Find the XDK Direct3D functions (draws, clear, resolve, tiling, swap, shader create, constant writers, ring, fences) in a new game image automatically, then confirm them.
---
# Xdk hook discovery

1. Decoded image + register: port/logs/default_image.bin, port/generated/default/<game>_register.cpp.
2. `python tools/re/xdk_sigs.py match port/logs/default_image.bin port/generated/default/<game>_register.cpp -o artifacts/xdk_match.tsv`.
3. exact = same XDK revision (Conan self-test: all 26 hook functions exact). Still confirm each hook target: callers, argument use (callargs.py), PM4 constants (pm4scan.py: DRAW_INDX 0xC0012201/0xC0032201), device offsets (docs/XDK_D3D_NOTES.md).
4. Late-XDK game (many fuzzy/missing): `python tools/re/xdk_layout.py port/generated/default/<game>_register.cpp --image port/logs/default_image.bin -o artifacts/xdk_layout.tsv` names D3D, CRT and xapilib functions by size layout against the 2012 XDK table (also gives setjmp/longjmp and the [rexcrt] candidates).
5. Still fuzzy/missing = different XDK revision: find semantically (PM4 headers, device-offset writers, strings, call patterns) and add to a game symbol .tsv; rebuild signatures for future games with `xdk_sigs.py build`.
6. Table of role -> address in docs/RENDERER_ANALYSIS.md section 3.
