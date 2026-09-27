# Project State - <GAME>

> Handoff between sessions. Keep current. Last update: <date> (<what changed>).

## Current status
- Phase (docs/NATIVE_PORT_PLAYBOOK.md): 0
- Game: <title>, title ID <XXXXXXXX>, XEX SHA-256 <...>, modules: <...>
- Renderer path: legacy Xenos | native A/B | fully native
- Last A/B (swap: PSNR): -
- Performance (scenario, build): -
- Frame bound by (PERFORMANCE_GUIDE "Where is the frame bound?"): guest | worker | GPU | - ; guest ceiling: -

## Game facts (docs/ANY_GAME_CHECKLIST.md)
| Item | Answer |
|---|---|
| Revision: XEX / .xexp SHA-256, title update used? | |
| XDK revision / xdk_sigs.py or xdk_layout.py result | |
| Extra modules (.xex/.dll) | |
| setjmp / longjmp addresses | |
| `[rexcrt]` groups applied (memory/string, heap, file) | |
| Codegen flags applied (G1) | |
| Renderer strategy (XDK hooks / engine-level) and why | |
| Shader source (raw containers / compressed packages / runtime harvest) | |
| Presents every N vblanks (fps cap rule) | |

## Build
```bash
source scripts/dev_env.sh
bash bench/build.sh nr        # RelWithDebInfo (dev)
bash bench/build.sh nr-rel    # Release
```

## Run / benchmark
```bash
bench/run_safe.sh 200 <name> bench/scenario_<x>.txt <exit_s> [--cvar=value]
python tools/bench_summary.py artifacts/profiles/<name>.csv 40 50 90
```

## Key addresses (see docs/RENDERER_ANALYSIS.md)
| What | Address | Confidence |
|---|---|---|

## Native renderer status
| Area | Native | Validated | Notes |
|---|---|---|---|

## Next actions (in order)
1.
