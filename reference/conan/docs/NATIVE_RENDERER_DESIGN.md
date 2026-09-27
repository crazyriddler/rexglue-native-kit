# Native renderer design (Conan)

Status: skeleton in progress (2026-09-24). Legacy Xenos path stays the default until
the native path reaches visual parity on the regression scenarios.

## Why (measured, see EXPERIMENT_LOG EXP-006..008)
- Legacy frame (scenario_jungle, 1200-draw cluster): 9.46 ms after the present fix; the CP
  thread is CPU-bound (~9.3 ms: UpdateBindings ~36%, packet decode, EDRAM/RT cache,
  submissions) and replays predicated-tiling passes per tile (1209 host draws for 616 guest draws).
- With Xenos translation disabled (`--gpu_null_draws`) the game runs at 3.97 ms (guest-bound).
- Target: native submission of ~616 draws + ~30 resolves in <= 1.5 ms CPU on the render thread.

## Architecture

```text
guest render thread (recompiled game + XDK D3D)
  D3D state setters (recompiled, untouched) -> guest D3DDevice shadow (0x5700 bytes @ *0x82C81A64)
  hooked: Draw*/BeginVertices/Resolve/Clear/Swap/SetRenderTarget/SetDepthStencilSurface/
          BeginTiling/EndTiling/SetPredication (+ resource create/lock later)
     -> NativeRenderer (conan-port/src/native/)
          reads device shadow at draw time (reblue model)
          host resources: textures (untile + convert), VB/IB (byte-swapped copies, cached by
          guest range + write-watch), host RTs for guest surfaces, resolves -> copies
          shaders: offline XenosRecomp DXIL keyed by container hash (artifacts/shaders)
          PSO cache keyed by (VS, PS, decl, RT formats, blend/depth/raster state)
     -> D3D12 command list on the SDK's device/queue (D3D12Presenter::provider())
  Swap -> NativeRenderer::Present -> Presenter::RefreshGuestOutput (copy native backbuffer)

Xenos plugin: loaded with --gpu_null_draws=true (sync-only PM4 consumer: WAIT_REG_MEM, fences,
interrupts, swap interrupt), so all guest-visible D3D semantics (fences, BlockUntilIdle,
callbacks) keep working unchanged. Later optimization: stop emitting draw PM4 at all.
```

Tiling: native path ignores predicated tiling (renders each tiled pass once at full resolution);
EndTiling's per-tile resolves become single copies.

## Staging
1. N1 skeleton: `--native_renderer` switches Xenos to sync-only and presents a native clear
   color through the presenter. (proves device/queue/present plumbing)
2. N2 frame structure: host RTs for guest surfaces, Clear, Resolve (copy), Swap from the real
   front-buffer texture; per-pass debug colors.
3. N3 first real pass: HUD (BeginVertices, 15 draws/frame) - shaders from the XenosRecomp corpus,
   textures (UI atlases), blend state; validate vs legacy screenshot.
4. N4 post-process chain (12 draws, 10 resolves), then opaque/decal/character/shadow passes.
5. N5 performance: PSO/descriptor caching, buffer caching, no PM4 emission for draws.

## Validation
`bench/run.sh ... --bench_screenshot_times=...` on legacy vs native, image diff
(tools/imgdiff.py, to be written); per-pass isolation by disabling passes.
