# Native renderer architecture (the design that worked)

Reference implementation: `reference/conan/port/src/native/` (~9k lines). Final result on
Conan: no Xenos/Xenia GPU emulation loaded, frame-exact A/B vs Xenos 47-58 dB (practically
pixel-exact), 4.0 ms/frame unlocked vs 4.77 ms legacy (guest-bound ceiling 3.97 ms),
arbitrary internal resolution, 4096 shadows, MSAA options, enhancements, PSO precompile.

## 1. Big picture

```text
recompiled game + statically linked XDK D3D (untouched: state setters keep writing the
guest D3DDevice shadow and the XDK command segment)
   │  hooks on ~25 XDK functions (draws, clear, resolve, tiling, swap, shader create,
   │  constant writers, ring segment switch, fence wait) + game pass functions
   ▼
CAPTURE (guest threads, cheap): WorkCmd = {type, pass, PM4 words since last cmd,
   captured guest ranges (device blocks, surfaces, decl, rects, inline vertices),
   buffer upload plans}  -> batches of 32 -> worker queue
   ▼
RECORD (1 worker thread): Pm4Mirror scan -> state -> host surfaces / textures / buffers
   -> PSO (pipeline cache) -> D3D12 command list on the renderer's OWN direct queue
   ▼
PRESENT: swap hook -> copy native output into the presenter's guest output (presenter
   queue, cross-queue fence wait) -> SDK D3D12 presenter (vsync / latency waitable)

NativeGraphicsSystem (IGraphicsSystem, installed in OnPreSetup):
   D3D12 presenter + GPU MMIO (0x7FC80000) + sync-only PM4 consumer (fences, interrupts,
   WAIT_REG_MEM, swap counting, gamma ramp) + vblank thread. No Xenos plugin.
```

Principles that made it work:
- **Hook the XDK API, read the device shadow.** The XDK keeps (almost) all state in the
  guest `D3DDevice` struct and only emits PM4 in the draw prologue. Hooks call the
  original (`__imp__sub_X`) first so the draw's state flush lands in the command segment.
- **PM4 mirror for everything that bypasses the shadow.** Shader literal constants
  (LOAD_ALU_CONSTANT at shader bind), ring constant writers, inline SET_CONSTANT and
  inline type-0 blocks never touch the shadow. Parsing the XDK command segment between
  draws gives a register file equal to Xenia's (0 differences). This is more robust than
  hooking each emitter.
- **The GPU side of the guest contract stays emulated minimally**: the guest waits on
  fences, read-pointer write-back, interrupts and vblank. NativeGraphicsSystem consumes
  the PM4 stream for those only (draw/shader/texture packets skipped).
- **Offline shaders**: XenosRecomp (patched) -> DXIL, embedded in the exe, looked up by
  container hash registered at the XDK shader-create hooks.
- **Validate frame-exactly** against Xenos at the same guest swap number (A/B mode loads
  Xenos as the reference while the native renderer renders offscreen).

## 2. Guest-side capture (hooks)

| Hook (XDK role) | Capture |
|---|---|
| DrawVertices / DrawIndexedVertices / DrawVerticesUP / BeginVertices+EndVertices | call original, then `WorkCmd{kDraw*}`: PM4 words since the last command, device blocks (fetch shadow 0x480-0x780, register shadow 0x2880-0x2A50, decl, RT/DS/IB/VB/texture pointers 0x3080-0x31A0, viewport), surfaces, vertex decl, inline vertex data; buffer plans |
| Resolve | rect/point/dest texture/clear values; copy state from RB_COPY_* |
| BeginTiling / EndTiling / SetPredication | tiling rects; predicated tiling is ignored natively (each pass rendered once at full size); EndTiling's per-tile resolves go through Resolve |
| Clear (XDK internal draw) | explicit clear command (XDK flag bits) |
| Swap (+ game Present) | frame boundary; worker flush + wait for frame N-1 (lag 1) |
| CreateVertexShader/CreatePixelShader | container hash -> shader registry (object address -> hashes, is_vertex) |
| Ring segment switch / large alloc | parse the old segment tail, resync the mirror |
| VB/IB Unlock | dirty ranges (64 KB page index) for the buffer tracker |
| Game pass functions | `PassScope` sets the current pass index (per-pass stats, pass-specific logic) |

Buffers: `TrackedBuffer`/`PlanBuffer` on the guest side decide what each draw uploads
(vertex range from start/count or the index min/max scan, SSE4.1). Only dirty bytes are
copied (CopyBufferRegion into persistent default buffers).

## 3. Worker-side state and resources

- **Pm4Mirror** (`pm4_mirror.cpp`): type-0/1, SET_CONSTANT(0x2D), SET_SHADER_CONSTANTS,
  LOAD_ALU_CONSTANT, INDIRECT_BUFFER (followed, size-capped). Versions per VS/PS constant
  block let constant buffers be reused while unchanged. NaN lanes are sanitized to 0 (SIMD).
- **Surfaces** (host render targets): key = EDRAM base + format + width/height + log2(samples)
  (depth has no format distinction). Scale per surface (render scale, shadow scale for
  square targets created in the shadow pass, 1:1 for targets < 16 px, bloom scale for
  small post targets). EDRAM model: each surface records its tile footprint and a write
  sequence; on bind, a surface overwritten by another non-co-bound surface since last use
  is cleared (color 0, depth 1) or bit-reinterpreted (same base, other 32 bpp format,
  `edram_alias.hlsl`). Checks only when the bind set changes.
- **Resolves**: host textures keyed by (guest physical base, format, size); "latest per
  address" binding. Color MSAA -> ResolveSubresourceRegion (AVERAGE, like Xenos). Depth ->
  R32F via `depth_copy.hlsl` (sample 0) plus an RGBA8 view of D24S8 for 8888 fetches (not
  for shadow maps). Tiled resolves keep tiles at their screen position.
- **Textures** (`texture_decode.cpp`): fetch constant -> SDK TextureInfo -> linear copy or
  Xenos 2D tiled addressing (port of Xenia XenosTextureTiledAddress2D; the legacy
  `texture_conversion::Untile` is WRONG), endian swap per block, DXGI format + component
  mapping from the fetch swizzle; cubes (6 faces), EXPAND formats as float16. Cache entry
  = guest range + content hash; revalidated through guest physical write watches (per
  4 KB page sequence). Fence-tagged deferred release of SRVs/resources.
- **Descriptors**: one bindless SRV heap (slots 0/1/2 = null 2D/3D/cube), sampler heap
  keyed by fetch sampler bits (+ anisotropy option, + LOD bias). Fixed slots allocated
  once for per-frame blits.
- **Constants**: b-registers from the mirror (VS/PS float files 4 KB each), plus a
  **shared constant block** (b2 space4) the XenosRecomp patch reads:

  | c | fields |
  |---|---|
  | c16-c17 | Xenos bool file (VS 0-127, PS 128-255) |
  | c18 | misc; c18.y = g_HalfPixelOffset |
  | c19-c26 | 32 loop constants |
  | c27 | g_ScreenXform (pixel -> NDC for screen-space draws) |
  | c28 | x spec bits (1<<1 alpha test, 1<<3 alpha-to-coverage, 1<<4/5 soft particles rgba/alpha), y g_PixelPosScale, z g_ShadowAtlasTexelScale, w g_ShadowSoftness |
  | c29 | xy g_SoftParticleW, z soft particle distance, w texel scale |
  | c30 | x soft-particle depth SRV |

  Texture descriptor index bits: 0-14 SRV index, 16-27 TEX_SCALE (8.8 fixed, 0 = 1:1; keeps
  offsets/weights in guest texels for upscaled resolves), 28 smooth (cubic) magnification,
  31 PWL gamma.
- **Fixed-function state**: RB_BLENDCONTROL/COLOR_MASK, RB_DEPTHCONTROL + stencil ref/mask,
  PA_SU_SC_MODE_CNTL (cull, poly offset from PA_SU_POLY_OFFSET_* in the mirror),
  RB_COLORCONTROL alpha test (spec bit + RB_ALPHA_REF) and alpha-to-mask ->
  AlphaToCoverageEnable, viewport (floats, half-pixel offset, screen-space VTE), scissor
  (window scissor + offset clamped by screen scissor, not during tiling).
- **Primitives**: quad lists -> index expansion; rect lists -> CPU expansion with Xenia's
  longest-edge diagonal rule; strip restart 0xFFFF/0xFFFFFFFF; >65535 splits handled by
  the XDK.
- **PSO**: key = shaders + decl + RT/DS formats/samples + blend/depth/raster;
  `PipelineCache` records every PSO as a pointer-free record, appends to
  `<game>_pipelines.bin`, and precompiles the embedded base (RCDATA 3) + local file at
  startup on 1-4 below-normal threads; GetPipeline waits for an in-flight compile.
- **Frame/queues**: own direct queue; per-frame command allocators; 128 MB per-frame upload
  ring + overflow pages retired with the frame fence (peak measured 59 MB); present copy on
  the presenter queue; per-frame output targets with a present fence.

## 4. NativeGraphicsSystem (guest GPU contract without Xenos)

- MMIO range 0x7FC80000: CP_RB_WPTR kick wakes the consumer; register reads for the few
  the guest polls.
- PM4 consumer (sync only): read-pointer write-back (big-endian), EVENT_WRITE_SHD/EXT,
  MEM_WRITE, REG_TO_MEM, COND_WRITE, scratch write-back, WAIT_REG_MEM (blocking on an
  event set by vblank/MMIO writes, memory polls sleep 100 us), INTERRUPT, XE_SWAP counting,
  DC_LUT gamma ramp, bin predication. Registers >= 0x2000 and constant loads are skipped
  (the renderer has them from its own mirror). Stores use GpuSwap(value) in host order.
- GPU progress generation + condition variable: the hooked XDK fence poll sleeps until
  the exact fence condition instead of spinning (1 ms cap so the XDK hang check runs).
- Vblank thread: rate = 2 x fps_limit for games that present every second vblank (check
  your game), or the unlocked rate for benchmarks; delivers at most one vblank per wake.
- Gamma: guest 256-entry DC_LUT applied in the final blit.

## 5. Options and enhancements (all generic, all verified)

render_scale (float 1-4, one rounding function `HostPx`), shadow_quality 1/2/4, MSAA
Off/4x/8x for surfaces the game multisamples, anisotropic filtering, FPS limit, VSync,
full-resolution scene (always on when the game renders its scene below the output size),
smooth effects above native res (always on), foliage alpha-to-coverage (needs MSAA),
FXAA (before the HUD pass), SSAO (camera from the game's inverse view-projection shader
constant; apply before transparents), bloom quality, dithering, shadow smoothing
(rotated PCF), soft particles.

## 6. Diagnostics kept in the build (cvar-gated, no hot-path cost when off)

`native_ab_mode` (+ `native_ab_swaps`, `native_dump_swap`), `native_dump_frame_at_s` /
`native_dump_after_pass`, `native_trace_frame_at_s` (every RT bind, resolve, draw with
shaders/state/constants), `native_skip_ps=<hash>`, `native_skip_draws`, `native_pass_mask`,
`native_debug_*`, `native_gpu_pass_timing`, `native_gpu_breadcrumbs`, hang watchdog
(`native_hang_watchdog_s`: all-thread stacks when swaps stop), `d3d_capture_out`,
`native_dump_textures_dir`, per-pass drawn/skipped stats.
Rule: no clock queries, logs or scans in per-draw paths unless behind a cvar.

## 7. Invariants (do not "optimize" these away)

The renderer reproduces a console frame whose meaning depends on submission order. These
rules come from bugs that were fixed on Conan; proposals that break them are rejected in
docs/STRATEGY_REVIEW.md.

- **Submission order is semantic.** Guest surfaces alias by EDRAM base; a resolve copies
  what was drawn so far; clears are limited to rects; stencil and blending accumulate. No
  sorting by PSO/material, no instancing that merges draws across state changes, no moving
  work to another queue unless the dependency is explicit.
- **Never drop or substitute a draw the game submitted.** A draw skipped once (cold PSO,
  missing texture, culling) can break the game permanently (EXP-005: exposure init ->
  black scene). A missing pipeline or texture is waited for; coverage and precompile make
  the wait not happen.
- **No host-side culling of submitted draws.** The game already culls; occlusion queries
  are game-visible (EXP-013).
- **State maps exactly.** No "nearest canonical" blend/depth/raster states.
- **Game-visible semantics** stay: pixel centers, PWL gamma, NaN-as-zero constants, 7e3 HDR
  with alpha, EDRAM reinterpretation, MSAA sample count of the guest.

## 8. Cheap D3D12 hardening for every port (not in the Conan reference yet)

Generic, low-risk additions to make during phase 6 of the next port (and back-port into
the reference when it is next built). None changes pixels; confirm with one A/B run.

| Item | Why | How |
|---|---|---|
| DRED (auto-breadcrumbs + page-fault reporting) | Device-removed reports with the last GPU operation, always available in Release, instead of the per-draw MARKER_OUT breadcrumbs (opt-in, costly on AMD) | `D3D12GetDebugInterface(ID3D12DeviceRemovedExtendedDataSettings)` before device creation (the SDK creates the device: add it there, document in sdk/KIT_SDK_CHANGES.md); dump `ID3D12DeviceRemovedExtendedData` in the existing device-removed log path; cvar-gated if any cost is measured |
| PIX event per guest pass | Captures and GPU timing readable by pass name | `PIXBeginEvent/PIXEndEvent` (WinPixEventRuntime header-only markers or `ID3D12GraphicsCommandList::BeginEvent`) in PassScope, only when `native_gpu_pass_timing` or a capture is active |
| `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED` | No driver clear on surface/resolve/texture creation (level-load hitches) | On committed resources that are fully written before first read (RTs cleared by the game, resolve destinations, uploaded textures). Not on buffers read before a full upload |
| Root signature 1.1 | Lets the driver assume per-draw CBV data is static while set | `D3D12_VERSIONED_ROOT_SIGNATURE_DESC` 1.1, `DATA_STATIC_WHILE_SET_AT_EXECUTE` on the 3 CBVs, descriptor ranges `DESCRIPTORS_VOLATILE` (the bindless heap changes while bound) |
| Clear/resolve rect normalization | Many small rects defeat fast clears; LostOdysseyRecomp's per-tile depth clears (720 rects) cost 4K from 43 to 7.5 fps | Union the rects exactly (never grow them); a union equal to the surface -> NumRects = 0 full clear |
| Barrier batching | Buffer uploads issue 2 transitions per buffer per batch | Collect upload copies of a batch first, one barrier array before and after |
| GPU-based validation run | Catches state/descriptor misuse the debug layer misses | `--d3d12_debug=true` + `SetEnableGPUBasedValidation` on one scenario per milestone (slow; never in benchmarks) |
