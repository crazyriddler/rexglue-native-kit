# Project State - <GAME>

> Handoff between sessions. Keep current. Last update: <date> (<what changed>).

## Current status
- Phase (docs/NATIVE_PORT_PLAYBOOK.md): 0
- Game: <title>, title ID <XXXXXXXX>, XEX SHA-256 <...>, modules: <...>
- Renderer path: legacy Xenos | native A/B | fully native
- Last A/B (swap: PSNR): -
- Performance (scenario, build): -

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
