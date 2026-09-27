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

## SDK self-tests (after any change under `sdk/`)

The SDK carries its own suites; run them before rebuilding a port on a modified SDK.
```bash
cd sdk
cmake --preset win-amd64 -DREXGLUE_BUILD_TESTS=ON        # Linux: linux-amd64 (see below)
cmake --build out/build/win-amd64 --config Release --target rexglue ppc_tests unit_tests
out/win-amd64/Release/ppc_tests      # PPC instruction semantics through the real recompiler
out/win-amd64/Release/unit_tests     # core, kernel, memory, codegen writer, timer queue
```
- `ppc_tests` assembles `sdk/tests/ppc/asm/*.s` with `tools/binutils`, recompiles them with
  `rexglue recompile-tests` and checks registers/flags. Expected: all pass (2026-09-27:
  1463 cases, 5745 assertions). **An instruction bug is fixed by adding a case here first**
  (format: `#_ REGISTER_IN` / `#_ REGISTER_OUT`, carry via `adde r6, r0, r0`), showing it
  fails, then fixing the builder.
- `unit_tests`: expected all pass except the known upstream Linux failures listed in
  `sdk/KIT_SDK_CHANGES.md` (chrono 1601 epoch, output-stamp path escaping).
- Linux (e.g. a cloud check without Windows): clang >= 19 (clang 18 + libstdc++ 13 lacks
  `std::expected`: pass `-DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20`) and
  SDL's X11/Wayland/ALSA/Pulse development packages; `tools/binutils` Linux binaries are
  executable in git.
- `python -m pytest scripts/tests` covers the kit's own Python tools and the sraw/srad
  emitter check without building the SDK.

## Additional validation layers

These complement the Xenos A/B; none replaces it. The shader harness, golden dumps and
capture replay are specified but not built yet (STRATEGY_REVIEW §6); build each when its
need appears.

### Shader numeric harness (open question Q-S1 of the reference)
A/B proves the shaders a scenario uses. For the rest of the corpus: run each translated
shader on WARP (D3D12 software device, deterministic) with seeded random constants/inputs
and compare with the SDK's Xenos shader interpreter
(`sdk/src/graphics/pipeline/shader/interpreter.cpp`) on the same inputs. Tolerance in ULPs;
report per shader in SHADER_CATALOG.md ("numeric: ok / diff / n.a."). Run it whenever the
XenosRecomp patch changes. Build it once as a kit tool (`tools/shaders/`), it is generic.

### Native golden dumps (regression without the plugin)
After a milestone passes A/B, store `native_dump_swap` outputs for the scenario swaps as
goldens (`artifacts/goldens/<scenario>/<swap>.png`, not in git if large). Renderer
refactors then compare native-vs-golden (expect identical or > 60 dB) in one run, without
loading Xenos; a real change of output still goes through A/B.

### Capture replay (renderer without the game)
WorkCmd batches are self-contained (captured guest ranges + buffer plans; texture memory
is read live, so a replay file must also snapshot the texture/constant pages the frame
reads). Serializing N frames lets you profile the recording worker in isolation, bisect
renderer regressions quickly and run the renderer under GPU-based validation without the
game. Medium effort; build it when a second game is ported or the worker becomes the
bottleneck.

### Release gates (scriptable from the logs)
| Gate | Source |
|---|---|
| A/B >= 45 dB on every scenario swap | ab_multi.sh |
| 0 PSOs compiled during play (after precompile) on the scenario set | "PSO #n created" log lines (written only for pipelines not served by the cache); Release logs are off: use the dev build or `--log_level=debug --log_file=<name>` |
| 0 frames > 50 ms outside loading screens | perf CSV + scenario timeline |
| 0 hangs in `repro_freeze.sh` N runs + long session | watchdog |
| CPU at the frame cap not worse than the previous release (paired) | opt_baseline.sh |
| No D3D12 debug-layer errors on one scenario, one GPU-based validation run per milestone | `--d3d12_debug=true` log |
