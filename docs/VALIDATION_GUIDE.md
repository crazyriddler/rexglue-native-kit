# Validation guide

## Frame-exact A/B against Xenos (the backbone)

`native_ab_mode=true`: the Xenos plugin renders and presents normally; the native renderer
renders the same guest frames offscreen. Compare by **guest swap number** (the CP lags the
CPU, time-based screenshots differ).

```bash
bench/ab_multi.sh <name> bench/scenario_x.txt <exit_s> 3000,4500,6000 [cvars]
# -> PSNR per swap + artifacts/captures/<name>_sheet.png (native | xenos | diff x8)
SCEN=bench/scenario_x.txt bench/ab_swap.sh <name> <swap> <exit_s>   # one swap, full dumps
```

Reference numbers (Conan, scale 1): menus 58 dB, gameplay 50-51 dB, cinematic 47 dB.
Below ~45 dB something semantic is wrong; ~30-35 dB = sub-pixel shift / MSAA / gamma class;
10-20 dB = missing or extra draws.

Enhancements and scale > 1 are validated visually (dumps / window captures) plus A/B at
original settings to prove nothing regressed.

## Diagnosing a difference

1. Contact sheet diff x8 -> region; mean signed difference per region (tint vs edges).
2. `--native_skip_ps=<hash>` / `--native_skip_draws=P:a-b` / `--native_pass_mask` to find
   the draw; `--native_debug_no_ztest`, `--native_debug_buffers_always_dirty`,
   `native_texture_watch=false` to classify.
3. `--native_trace_frame_at_s=T` logs every RT bind, resolve, draw (VS/PS, decl, fetch
   constants with bound host resources, viewport, stencil, PSO state, constants).
4. `--native_dump_frame_at_s=T` / `--native_dump_after_pass=N` + `tools/native_dump_to_png.py`
   for every surface/resolve; `--native_dump_texture_addr=<hex>` for textures.
5. Legacy side: `--d3d12_readback_resolve=true` decodes Xenos resolves of that frame
   (64 bpp readbacks are unreliable).
6. RenderDoc (launch under `renderdoccmd capture`) for GPU-level inspection.

Known acceptable differences: hardware alpha-to-coverage patterns, PCF dither, isolated
edge pixels (< 0.5% of pixels differing by > 2 levels).

## Stability

- `bench/repro_freeze.sh <runs> <scenario>` classifies runs (HANG via the hang watchdog,
  which logs all thread stacks). Turn "sometimes" into a rate before and after a fix.
- Long session scenario (10 min pseudo-random walk/fight) before a release.
- Crash dumps: ProcDump `-e -ma` (unhandled only; `-e 1` triggers on SetThreadName) +
  `windbg-tool.exe` for symbolized host stacks.

## Coverage

Maintain scenarios for: boot + FMV, menus/options/load, new game (cinematic, tutorial HUD),
a saved level, combat/effects, pause menu + resume, a level transition, long session.
The user's own saves folder is off-limits: create bench saves inside the bench user data.
Window captures: `tools/capture_window.py` (own process only, PrintWindow with
PW_RENDERFULLCONTENT; screen copy does not capture D3D content reliably).
