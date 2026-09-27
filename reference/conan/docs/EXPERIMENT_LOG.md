# Experiment Log

Append experiments chronologically. Do not delete failed experiments; they prevent repeated dead ends.

## Template

### EXP-000 - Title
- Date/commit:
- Question:
- Hypothesis:
- Change/instrumentation:
- Build/run scenario:
- Metrics/evidence:
- Result:
- Conclusion:
- Keep/revert/follow-up:

### EXP-001 - Build against workspace SDK
- Date/commit: 2026-09-24 / 7c058f8
- Result: clean configure + build in `conan-port/out/build/nr` (905 steps, ~2 min). Keep.

### EXP-002 - Automated reproducible scenario
- Change: autoinput_script / bench_exit_after_s / bench_screenshot_times cvars; bench/run.sh.
- Result: boot to in-game without focus/OS input works reliably (3/3+ runs). Attract mode at title is a video (6-18 draws), not usable as in-engine benchmark.

### EXP-003 - First in-game baseline (legacy Xenos)
- Metrics: 23.3 ms avg, CP busy == frame time, GPU 6.4 ms, 1753 draws. See BENCHMARKS.csv.
- Conclusion: PM4/Xenos translation on the CP thread is the bottleneck -> native renderer directly targets it.
- Side finding: "Begin/End viz query" logged at INFO for every query (~100 MB/min logs). Demoted to DEBUG.

### EXP-004 - Black in-game scene triage
- Symptom: scene black (pixel values 1-2), HUD renders, 1753 draws/frame.
- Tested, no change: clear_memory_page_state=true, occlusion_query_enable=false, readback_memexport=true, force_convert_quad_lists_to_triangle_lists=false, resolution_scale=3, vsync=true.
- Status: open. Awaiting user's manual check with dist/ build.

### EXP-005 - Black 3D scene: root cause = async pipeline creation dropping draws on a cold shader cache
- Symptom (EXP-004): scene black, HUD fine, game alive (pause/skill menus open, draw count changes when walking).
- Bisection: pre-perf SDK (c94f5eb + perf-backup patch, worktree ../rexglue-sdk-oldperf) ALSO black -> not a perf-work regression.
- Discriminator: black only when `cache/shaders/shareable/545107DA.*` is absent/empty. Second run with the cache written by the cold run renders fine. `--async_shader_compilation=false` renders fine on cold cache.
- Cause: with async compilation, IssueDraw returned early (dropped the draw) while its pipeline was pending. A one-time draw initializing persistent/feedback state at level start (probably adaptive exposure) is lost forever -> permanently black scene. Warm cache pre-creates pipelines at startup so nothing is dropped.
- Fix: `PipelineCache::WaitForD3D12PipelineByHandle` (new `Pipeline::creation_finished` flag); IssueDraw waits by default (`async_pipeline_skip_draws=false`). Shader translation now always synchronous (`async_shader_translation=false`) - equivalent latency since the draw waits anyway.
- Verified: cold cache -> scene renders (nr_cold_wait2 screenshots).
- Pitfall found: CMake copies SDK DLLs into the exe dir only when conan.exe relinks -> use `bench/build.sh <builddir>` which always syncs DLLs. An intermediate test was invalid because of this.

### EXP-006 - Baseline scenario + where the time really goes (legacy Xenos, Release nr-rel)
- Scenario: `bench/scenario_jungle.txt` (boot -> load "Chance Meeting" BI_03 jungle -> scripted walk), window t=50..90s, warm shader cache (`bench/userdata_template/cache`), rs1, no vsync, 1280x720, RTX 4080, 280 Hz monitor.
- 3 runs: 13.93 / 13.97 / 14.06 ms avg (p95 18.0), ~1195 draws, 30 resolves, 36 RT switches, 3 submissions/frame, GPU busy 4.1 ms.
  Other runs of the same script land in a second cluster (~1330 draws, 17.2-17.5 ms) -> path not fully deterministic; compare only within-cluster or use many runs.
- `cp_busy_us` == frame time is MISLEADING (includes blocking inside ExecuteCommandLists). Real CPU (QueryThreadCycleTime): CP thread 8.1 ms/frame, guest VdSwap thread 10.2 ms/frame.
- Frame times are quantized at multiples of 3.571 ms = 1/280 Hz host refresh (10.75/14.3/17.9 ms). Not guest vblank (240 vs 1000 Hz vblank: same; 60 Hz vblank -> 33.3 ms: the game waits for 2 vblanks per frame, i.e. vsync=on in this port caps it at 30 fps).
- In-process sampler (`--sample_profile_out`, src/core/perf/sampler.cpp; symbolize with tools/symbolize_profile.py):
  - guest thread: ~70% inside Swap chain sub_822E8EB8 -> sub_822DF1D8 -> sub_822DE050 (spin-waiting on GPU progress), 27% blocked in NtWaitForSingleObjectEx (sub_824BA8E8 -> sub_822DDD80).
  - CP thread: 45% inclusive in EndSubmission -> ExecuteCommandLists blocked on a D3D12Core internal lock/wait (ntdll wait via msvcp_win); 36% exclusive in D3D12CommandProcessor::UpdateBindings (~4 us/draw); packet decode/WriteRegister small (~5%).
  - host_present_from_non_ui_thread=false: no change (still quantized).
- Conclusion: the frame is bounded by CP<->host-queue synchronization (quantized to display refresh) plus ~8 ms of CP CPU work, with the guest thread mostly waiting on the CP. A native renderer removes the CP thread, PM4 building and UpdateBindings; presentation pacing must be designed so submissions never block behind present.

### EXP-007 - Host Present blocked the D3D12 queue lock -> CP ExecuteCommandLists stalls (FIXED, -32..41%)
- Evidence: sampler on the UI/present thread: 91.5% inside IDXGISwapChain::Present -> D3D12Core -> win32u kernel wait (DWM/flip queue throttling at the 280 Hz refresh even with SyncInterval 0 + tearing). CP thread's ExecuteCommandLists path (D3D12Core 0x11120/0x11190 -> msvcp_win mutex wait) shares those frames => queue lock held across the blocking Present.
- Fix: swap chain created with DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, SetMaximumFrameLatency(2), UI thread waits on the waitable object *before* Present (no D3D lock held). cvar `d3d12_present_latency_waitable` (default true). ResizeBuffers passes the flag too.
- Result (scenario_jungle, Release, window 50-90s): ~1200-draw cluster 14.0 -> 9.46/9.51 ms avg (p50 8.6, p95 12.8); ~1375-draw cluster 17.3 -> 10.28 ms. Refresh quantization gone. CP thread now CPU-bound (9.3 ms CPU per 9.5 ms frame), guest swap thread CPU 5.6 ms. Visual output verified (waitable_1/shot_0070s.bmp).
- Next bottleneck = CP thread CPU (UpdateBindings etc.) -> exactly what the native renderer removes.

### EXP-008 - Guest D3D capture (M3) and the "null draws" ceiling
- Capture hooks (conan-port/src/native/d3d_capture.cpp, --d3d_capture_out): guest issues ~616 draws/frame (DrawIndexedVertices ~425, DrawVertices ~175, BeginVertices 15 = HUD, DrawVerticesUP 3) and ~29.4 resolves/frame across 17 passes; Xenos CP processes ~1209 draws/frame (predicated tiling replays the tiled passes per tile + EDRAM helper draws). Per-pass table in artifacts/captures/cap2.json. HUD = BeginVertices/EndVertices in "Render HUD". Only D3D draw fns + BeginVertices write DRAW_INDX (0xC0012201/0xC0032201).
- `--gpu_null_draws=true` (new cvar: CP consumes PM4 for sync but IssueDraw/IssueCopy return immediately): frame locked at 8.33 ms = game waits 2 vblanks at 240 Hz. With `--unlocked_vblank_rate=1000`: 3.97 ms avg (p95 5.2) - guest-bound ceiling. CP CPU 1.3 ms/frame in this mode.
- Conclusion: Xenos translation/emulation costs ~5.5 ms of the 9.46 ms frame; a native renderer with ~1-2 ms submission cost could reach ~4.5-5.5 ms (1.7-2x). The 120 fps cap at 240 Hz vblank must be revisited (raise unlocked_vblank_rate or pace natively).

### EXP-009 - Offline shader corpus pipeline (M4): extract, hash, XenosRecomp (patched), DXIL/SPIR-V
- One command: `bash tools/shaders/build_corpus.sh` (~40 s): fetch DXC v1.9.2607 -> tools/dxc; fetch zolaware/reblue-XenosRecomp@339af41 + `tools/xenosrecomp/patches/0001-conan-recomp.patch`, build `XenosRecompCorpus` (clang, SDK fmt/xxHash; no zstd/smol-v/DXC link); `extract_shaders.py`; `join_runtime_cache.py`; `build_catalog.py`.
- Extraction: 1161 game files scanned + decoded default.xex image: containers exist ONLY in shaders/shaders.stx: 1800 containers in 146 `.fxc` records, 591 unique by container hash (66 VS / 525 PS), 534 unique ucode.
- Hashes: container_hash = XXH3_64(container[0:virtualSize+physicalSize]) (= XenosRecomp/UR/re:Blue key); ucode_hash = XXH3_64(raw big-endian microcode bytes [virtualSize+shader.physicalOffset, +shader.size)) (= ReXGlue Shader::ucode_data_hash). Runtime join against bench/**/545107DA.xsh (126 shaders): PS 82/82 exact; VS 0 exact, 38 after vfetch normalization, 3 fuzzy, 3 unmatched -> XDK patches VS microcode at declaration bind time.
- Stock generic translation (reblue fork, no game defines): 234/591 compile. Failures: 188 undeclared loop constant `i16` (int loop constants unimplemented), 158 undeclared `b128..b138` (PS bools keyed by local index vs unified CF address), 11 translator asserts (relative constant addressing on singleton/unnamed registers). Also found silent bugs: cexec bN blocks executed unconditionally, const1Relative ignored, CondExecPredCleanEnd not ending.
- CONAN_RECOMP patch -> **591/591 translate and compile** (DXIL vs/ps_6_0 default-spec + lib_6_3 for 556 spec-constant shaders; SPIR-V 591/591). 330 shaders use relative constant addressing (light arrays), 328 use loops (PS loop consts 16/17 runtime, 31 literal in 7 shaders), 4 Bloom PS use unstructured switch(pc).
- Status: nothing visually validated yet (Q-S1). Next: numeric VS validation against the SDK ucode interpreter, capture vertex declarations (Q-S2), then native POC.

### EXP-010 - Native CPU texture decoder validation
- conan-port/src/native/texture_decode.cpp: fetch constant -> TextureInfo::Prepare (SDK) -> linear copy or Xenos 2D tiled addressing (port of Xenia XenosTextureTiledAddress2D; identical output to texture_util::GetTiledOffset2D) with CopySwapBlock endian handling -> DXGI format + SRV component mapping from the fetch swizzle. Validation dump: `--native_dump_textures_dir` (DDS per distinct texture bound during capture, fetch dwords in fetch.txt).
- Pitfall: legacy `texture_conversion::Untile` gave WRONG results (visibly shifted blocks in the HUD atlas); do not use it.
- Result: HUD atlas (linear DXT5 512x512, fetch 04000002 ...) matches game/textures/hud/UI_HUD_Player.dds pixel-exactly (mean abs error 0.0) after an 8-row offset that is an authored difference (the packaged in-memory atlas has an extra 8-px red strip on top). 105 textures decoded in the jungle scene; failures: 4 non-2D (cube/3D), 8 unsupported format (24_8 depth etc.).
- Remaining: cube/3D textures, 24_8/CTX1/DXT3A/DXT5A formats, resolve-target textures (come from native RTs, not guest memory).

### EXP-011 - M5 milestone: first natively rendered pass (HUD) without Xenos
- `--native_renderer=true`: Xenos in sync-only mode; HUD pass (BeginVertices/EndVertices, quad lists) rendered by conan-port/src/native/native_renderer.cpp:
  bindless root signature (XenosRecomp convention), DXIL from artifacts/shaders/dxil keyed by container_hash (shader_registry.cpp hooks the XDK creators 822E84C0/822E85D0), PSO from guest vertex declaration + RB_BLENDCONTROL0/PA_SU_SC_MODE_CNTL/RB_COLOR_MASK read through the recovered register shadow, textures via the validated CPU decoder + GPU upload, samplers from fetch constants, VS/PS constants/bools/loops byte-swapped from the device shadow, quad->triangle index expansion.
- Validation: artifacts/screenshots/hud_native_vs_legacy.png - health HUD geometry, texture, colors match the legacy Xenos render.
- Finding: device 0x318C/0x3190 are NOT VS/PS respectively in the obvious order - resolved via registry is_vertex flag (Q-R4 answered: 0x318C = pixel shader object for HUD pairs).
- Everything except HUD is still a placeholder clear in native mode.

### EXP-012 - Full-frame native path bring-up (all passes routed natively)
- Implemented: host RTs per guest surface (full untiled size while BeginTiling is active), resolves -> host textures keyed by (guest base, host format) with "latest per address" binding, clears on BeginTiling/Resolve, DrawVertices/DrawIndexedVertices (guest VB/IB byte-swapped + cached, invalidated on XDK VB/IB Unlock hooks), DrawVerticesUP, depth/stencil state, front-buffer blit (precompiled DXIL blit into R10G10B10A2 output), recording mutex (draws and Swap come from different guest threads).
- Bug found (debug-layer InfoQueue callback now logs into the game log): the game aliases one guest address for differently formatted resolve targets within a frame (0xFBF17000: HDR 16-16-16-16 and 8888); recreating the destination released resources still referenced by in-flight lists -> GPU queue stall (fence never completes, no TDR) -> guest stuck in XDK BlockOnFence (RPtr writeback) ~33 s into the scenario. Bisected with native_draws / native_resolves / native_debug_resolve_mode cvars (copies = culprit), fixed by (address, format) keys + fence-tagged deferred releases (Retire()).
- Frame-dump tool: `--native_dump_frame_at_s=60 --native_dump_dir=<dir>` + tools/native_dump_to_png.py.
- State (frame1 dump): depth pre-pass renders correct jungle geometry; HDR scene partially shaded; one pass (surface 404FFD00) produces fan-shaped garbage geometry; luminance chain (32_FLOAT 1x576/1x1) all zero -> tonemap/upscale black -> final output black apart from HUD; 1280x720 surface not cleared (stale menu text).
- Next: per-pass debugging (luminance chain shaders, garbage vertex path, HDR shading), then visual comparison vs legacy.

### EXP-013 - Native bring-up fixes: EDRAM aliasing, strip cut, viewport floats, occlusion culling
- Host RTs now keyed by EDRAM base + format + size (surface objects alias EDRAM: depth pre-pass / opaque depth share base 0; 8888 and HDR color share base 936).
- Index buffer +0x18 is a guest *virtual* address (XDK converts: low 29 bits, +0x1000 for 0xE0000000 view).
- Indexed strips need IBStripCutValue (0xFFFF/0xFFFFFFFF) - fixed the giant shard triangles.
- Device viewport struct (dev+0x3160) holds floats (SetViewportF); was read as u32 -> fallback 1280x720 on 1024x576 RTs.
- Occlusion queries: in native mode Xenos draws nothing, so viz queries returned 0 and the game culled ~95% of batches (26 of 616 draws reached the renderer). Native init now sets occlusion_query_enable=false (fake visible count).
- Debug tools added: --native_pass_mask, --native_flip_winding (winding was NOT the issue), per-draw vertex/viewport debug logs.
- State (frame5 dump): depth shows ground + trees; HDR target has foliage/ground but dark; a top-right rectangle of depth 0 (tile/clear issue); luminance chain still zero; Upscale/final output stale.

### EXP-014 - Native path performance pitfalls (146 ms -> 768 ms -> 20.8 ms)
- In native mode the guest issues the same ~627 draws/frame (capture + native together). Earlier "only 11 draws/frame" reading was wrong: the per-pass log fired only at menu frame counts.
- Profile: GetBuffer 90%. Two problems: (1) VBs keyed by fetch address (includes the per-draw stream offset) -> multi-MB copy per draw into shared pools; now cached per whole guest VB (vb object +0x18/+0x1C) with an offset view, key includes (decl, stride, offset % stride). (2) The byte swap was done in place in the UPLOAD heap (write-combined) - reads of WC memory are extremely slow. Swapping in a thread_local CPU scratch buffer then one sequential memcpy: 768 -> 20.8 ms/frame.
- Rule for this codebase: never read back from upload-heap mappings.
- Visual state (frame7): HDR target shows grass/characters; depth has a top-right rectangle at 0; luminance/tonemap/upscale chain still broken (final output shows stale loading screen).

### EXP-015 - Rect lists, depth-only draws, per-pass accounting
- Per-pass drawn/skipped log (time based, every 2 s) showed: shadow/depth/opaque passes skipped many draws for "shader" = draws with NO pixel shader (dev+0x318C == 0, depth-only); Deferred Shade, End Tiling, Painters Edge, Post Process, Upscale skipped for "prim" = Xenos rectangle lists (prim 8).
- Implemented: null-PS PSOs (depth-only), rect-list CPU expansion (4th corner = v1 + v2 - v0 on float attributes; DrawVertices rect lists read stream 0 through the 0xA0000000 physical view).
- Result: essentially all guest draws now reach the native renderer (few skips). But frame8 dump shows shard-like geometry again in the main scene surface and the final output still stale -> the newly enabled draw classes (depth-only via null PS, rect expansion) need per-pass validation. Luminance chain now computes values (1x576 = log(0.01) because its HDR input is zero; 1x1 = 0.3 clamp).
- Open: surface at EDRAM base 468 (post-tiling HDR target) stays zero -> "Resolve HDR Texture"/"End Game Render" overwrite the good End Tiling resolve (base 936) with zeros; need to verify the copy-back draw (pass 16) and whether base 468 should alias base 936 contents (EDRAM 4xMSAA vs 1x layout).

### EXP-016 - Ring constants, guest physical addresses, frame trace (2026-09-25)
- GpuBeginShaderConstantF4 (822E7A48) writes depth-prepass matrices straight into the command ring; hook records the ring pointer and FlushRingConstants mirrors it into the device constant shadow before the next constant upload.
- Texture objects carry *virtual* addresses (0xE0000000 view, +0x1000 physical offset), device fetch slots carry *physical* ones -> resolve-texture lookups never matched. `GuestPhysical()` normalizes resolve dest, texture fetch base and the front buffer. Result: the post chain (luminance, bloom, tonemap) works on the native scene (tonemapped10.png).
- Tools: `--native_dump_after_pass=N` (dump right after pass N), `--native_trace_frame_at_s=T` (log all RT binds, resolves, tiling events and per-draw VS/PS/decl for one frame), `--native_skip_draws=P:a-b`, `--native_buffer_cache_per_frame`, `--native_ring_constants`.
- Pitfall: run.sh passes the dump dir through a path with a space -> use a relative dir (`../../../../artifacts/...`).

### EXP-017 - Upscale / final output (screen-space draws, RT/DS size mismatch)
- Pass 25 (Upscale) is a DrawVerticesUP strip with PA_CL_VTE_CNTL = 0x400: viewport scale/offset disabled, positions already in pixels (0..1280 x 0..720), clipping disabled (PA_CL_CLIP_CNTL 0x90000).
- Fix: XenosRecomp patch adds `g_ScreenXform` (shared constants c27) applied to oPos in every VS under CONAN_RECOMP; host sets it to pixel->NDC when VPORT_X_SCALE_ENA is clear (and the D3D viewport to the whole target), identity otherwise. Corpus rebuilt (591/591).
- Second bug: output still covered only 1024x576. The upscale target (1280x720 RT, EDRAM 720) is paired by the guest with the 1024x576 scene depth buffer (EDRAM 0); D3D12 renders the intersection of all bound targets. Xenos does not care. Fix: depth surface lookup takes the RT0 size as a minimum (separate host surface when larger). Also RT slots 1-3 are bound only when their RB_COLOR_MASK nibble is non-zero.
- Result: final 1280x720 output shows the full upscaled frame + HUD (f21.png).
- Frame graph confirmed by trace (60 s): shadows p4 (2 resolves into 4096x2048 atlas at y=0/1024), tiling p7..p16 scene at EDRAM 936 (8888 for decals, HDR fmt 12 afterwards), p16 resolve 936 -> 1BF18000 + EndTiling, 4 copy-back draws into HDR@468, p20/p22 resolves 468 -> 1BF18000, post p23, upscale p25 into 1280x720@720, HUD p27, present resolve p28.

### EXP-018 - Shader literal constants (LOAD_ALU_CONSTANT at shader bind)
- Symptom: large flat "planes" in the scene; varies from run to run; present in the depth pre-pass; draw bisection unreliable because of the variation.
- Found: sub_822F75F8 (shader bind) calls sub_822F7408(dev, table, data) which emits PM4 type-3 LOAD_ALU_CONSTANT (0xC0022F00) packets loading the shader's literal ("def") constants from the shader object's physical memory into c252-c255 (VS) / c500-c511 (PS). These never pass through the device shadow, so translated shaders read stale constants.
- Fix: hook sub_822F7408, walk the {u16 vec4 reg, u16 dwords, u32 offset} table and copy the literal block into the shadow (read through TranslatePhysical(GuestPhysical(addr)); reading base+virtual 0xF8xxxxxx returned the wrong page). Values now sensible ("2 -1 1 3", "0 0.1 1 1000").
- Still open: planes/missing ground vary between runs (t=45 idle frame: run 1 has the ground in depth, run 2 misses it).

### EXP-019 - Texture cache correctness (dynamic textures, cubes, EXPAND formats)
- Textures were cached by fetch constant only: dynamic textures rewritten in place (fog table) went stale. Now each cache entry keeps the guest base-level range + XXH3 hash; small textures (<=1 MB) are revalidated once per frame, larger ones every 30 frames; changed content -> re-decode (SRV indices recycled through a fence-tagged free list).
- Cube textures decoded (6 faces, 4 KB aligned tiled slices) with TEXTURECUBE SRVs; SRV slots 0/1/2 = null 2D/3D/cube and the shared constants pass the per-dimension index (a 2D SRV read through the cube heap was undefined -> NaN risk).
- k_16/16_16/16_16_16_16_EXPAND = 16-bit float (Xenia maps them the same way). The 2048x2 fog table uses 16_16_16_16_EXPAND: before this it decoded to the null texture -> flat fog-colored "planes" everywhere.

### EXP-020 - Shader-based depth resolves (shadow atlas)
- Depth resolves now write R32_FLOAT textures through a tiny precompiled shader (src/native/shaders/depth_copy.hlsl): arbitrary source rect -> destination point. The 4096x2048 shadow atlas gets its 1024x1024 tiles -> sun shadows work (Conan's shadow on the ground).

### EXP-021 - Inline SET_CONSTANT uploads, NaN constants
- sub_82580358(dev, r4, r5, count) reserves a PM4 SET_CONSTANT packet (0x2D) and returns the data pointer the game fills: per-object light constants PS c64+ and skinning matrices VS c0-c159. Hooked like GpuBeginShaderConstantF4 -> sun lighting and Conan appear.
- The game leaves NaN in unused lanes (light c64.w); Xenos multiplies them away, HLSL does not -> NaN pixels (Conan black). Constants are sanitized (NaN -> 0) at upload, as the Xenos path's E041 fix.

### EXP-022 - Alpha test via runtime spec constants
- XenosRecomp emits `if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST) clip(oC0.w - g_AlphaThreshold)`; the default DXIL returned 0 -> no alpha test -> black foliage cards. Now g_SpecConstants() reads shared c28.x (g_SpecConstantsRuntime); the host sets bit 1 from RB_COLORCONTROL (enable bit 3, func bits 0-2; GREATER/GEQUAL exact) and the threshold from RB_ALPHA_REF. One DXIL per shader, no variants.

### EXP-023 - PM4 command-stream mirror + frame-exact A/B
- Several constant paths bypass the XDK shadow (literal LOAD_ALU_CONSTANT at shader bind, GpuBeginShaderConstantF4, inline SET_CONSTANT, inline type-0 blocks such as the SpeedTree tables c160-c223 emitted by many inlined helpers). Instead of hooking each emitter, src/native/pm4_mirror.cpp parses the XDK command segment (dev+48 = last written dword, dev+52 = limit) between draws: type-0/1, SET_CONSTANT(2), SET_SHADER_CONSTANTS, LOAD_ALU_CONSTANT, INDIRECT_BUFFER (followed, size-capped). Segment switches: sub_822DF848 / sub_822DF548 hooks parse the old tail and resync. Draw hooks now call the XDK original first so the draw's state flush is in the stream.
- Validation: at the same guest swap, the mirror's ALU/fetch/bool/loop constants equal Xenia's register file exactly (0 differences; only 4 CP-internal registers 0x21F9-0x21FC differ).
- A/B mode (`--native_ab_mode=true`): Xenos renders and presents normally, the native renderer renders the same frames offscreen; `--native_dump_swap=N` + `--bench_screenshot_swaps=N` give the same guest frame from both paths; with `--d3d12_readback_resolve=true` the Xenos resolve results of that frame are decoded from guest memory (xenos_*.raw). Tools: /tmp-style wrapper abs.sh (see PROJECT_STATE), tools/ab_diff.py, tools/ab_resolves.py. Caveat: 64 bpp readbacks decode blocky (tiled 64bpp decode or readback layout suspect) -> not trusted yet.
- Current A/B (swap 4140, jungle walk): same geometry/animation; remaining differences: native ~40% brighter after tonemap + strong yellow haze (bloom/luminance chain), foliage darker, grass missing, 1x vs 4x MSAA edges.
- Pitfall found: native init forced gpu_null_draws/occlusion off, so the first "A/B" screenshots were stale Xenos frames.

### EXP-024 - Post chain: EDRAM aliasing model, raw depth-stencil view, copy_dest_swap, gamma ramp (A/B 14 -> 23 dB)
Frame-exact A/B at swap 4140 (jungle walk), native final output vs Xenos screenshot:
| Change | PSNR |
|---|---|
| baseline after PM4 mirror | 14.2 dB |
| EDRAM aliasing model | 15.3 dB |
| tiling resolves in place | 19.5 dB |
| copy_dest_swap | 20.7 dB |
| display gamma ramp | 23.0 dB |
- Bloom haze = feedback: the bright pass clip()s dark pixels, so they keep the RT's previous content; the native 256x144 bloom RT (EDRAM 484) was never cleared and accumulated over frames. On Xenos that EDRAM is overwritten each frame by the HDR scene (base 468). Model: each host surface records its EDRAM tile footprint (pitch x MSAA samples / 80x16-sample tiles, 64bpp halves the tile width) and a write sequence; on bind, a surface whose range was written by another (non-co-bound, different base) surface since its last use is cleared (color 0, depth 1/stencil 0).
- The bloom stencil-mask pass fetches the depth-stencil resolve (k_24_8 memory) as k_8_8_8_8: depth resolves now also produce an RGBA8 view of the Xenos D24S8 word (R = stencil, G/B/A = depth bytes) chosen when the fetch format is 8888.
- p9 leaf G-buffer (tiled): per-tile resolves pass pDestPoint (0,0); keeping the tile at its screen position fills both halves (deferred foliage lighting samples it full screen). Verified visually (foliage now lit everywhere).
- RB_COPY_DEST_INFO is reset by the XDK right after each copy; the PM4 mirror now captures RB_COPY_DEST_INFO/BASE at copy-mode draws (RB_MODECONTROL = 6). copy_dest_swap (bit 24) is set for the leaf buffer (A8R8G8B8 destination fetched with swizzle ZYXW): resolve textures with swap get an R<->B swap composed into their SRV mapping (fixed blue grass / cyan leaves). RB_COPY_DEST_INFO values seen: 0x302 (8888, 8in32), 0x01000302 (swap), 0xF001 (16_16_16_16_FLOAT), 0xF202 (32_FLOAT).
- Present: Xenos (and Xenia) apply the guest 256-entry DC_LUT gamma table at scan-out; the native blit now applies it (IGraphicsSystem::GetGammaRamp256 added to the SDK interface). Tonemapped 1B808000 already matched Xenos within ~+4 levels; the remaining +15-20 was this ramp.
- Remaining: slight bloom excess (zeroing bloom_intensity improves PSNR by ~1 dB -> the Xenos mask behaviour depends on EDRAM garbage), 1x vs 4x MSAA edges.

### EXP-025 - Upload overflow, dirty-range dynamic buffers, constant reuse (native 82 -> 7.2 ms)
- Black first gameplay frames: UploadConstants failed once the 128 MB per-frame upload ring was exhausted (load spike) -> every later draw of the frame dropped. Overflow upload pages (64 MB+, retired with the frame fence) replace the failure. Zero skipped draws since.
- Performance was dominated by whole-VB re-uploads: the XDK Unlock hook invalidated entire multi-MB dynamic VBs appended with NOOVERWRITE locks, ~48 GB uploaded per 100 s run. Now entries are marked dirty (never dropped) and each draw re-uploads only the bytes it reads (vertex range from start/count, or min/max of the index range for indexed draws) into the existing default buffer with CopyBufferRegion; clean ranges tracked until the next Unlock. 48 GB -> 1.3 GB per run. Unlock invalidation indexed by 64 KB pages.
- UploadConstants was 43% of the render thread: VS/PS constants now come straight from the PM4 mirror (SIMD NaN sanitize, no per-register fallback) and the constant buffers are reused while the mirror's VS/PS constant versions are unchanged; per-slot fetch cache avoids GetTextureSrvIndex for unchanged fetch constants; texture revalidation every frame only for <=256 KB.
- Measurement pitfall: out/build/nr-rel/conan.toml contained native_renderer=true, so "legacy" Release runs were native. Fixed (copied from nr).
- Xenos-path finding (legacy perf): the E041 NaN sanitizer scanned the constants in the write-combined upload heap (read-back) -> 63% of the CP thread in UpdateBindings; now staged in cached memory.
- Numbers (Release, vblank 1000 Hz, t=50-90): legacy 4.77 ms (p99 7.3); native 10.03 ms before, 7.16/7.17/7.87 ms after (p99 ~10).

### EXP-026 - Intermittent GPU stall = shared direct queue
- Symptom: native GPU fence never completes (no TDR, no device removed), ~50% of 1000 Hz vblank runs, at game-content-dependent points. Breadcrumbs (WriteBufferImmediate MARKER_OUT after every draw/copy/clear into a CPU-visible custom heap + description ring, printed on fence timeout) showed the GPU stopped at unrelated trivial draws, not progressing for >5 s, VRAM flat (~380 MB), D3D12 debug layer clean, Xenos null-draw mode alone never stalls, disabling native presentation did not help.
- Fix: the native renderer submits on its own D3D12 direct queue. The copy into the presenter's guest output runs on the presenter's direct queue (Wait on the native fence, small per-frame list), output targets are per frame in flight with a present fence before reuse. 5/5 runs clean (was ~1/2 stalling). A first version that copied into the presenter resource from the native queue produced corrupted frames (presenter mailbox reuse is tracked on the direct queue timeline).

### EXP-027 - Gamma textures, polygon offset, EDRAM same-base reinterpretation (A/B -> ~31.8 dB)
- Beach sand rendered flat white. Bisection with `--native_skip_ps` found the terrain PS F9C88944 (two-layer blend by vertex alpha). Its textures decode correctly; the cause is the fetch sign fields: RGB = GAMMA (dword0 bits 2-7 = 3), which Xenos linearizes when sampling (PWL ~2.2). Gamma textures now get TYPELESS resources + sRGB SRVs (BC1/2/3, RGBA8/BGRA8); sign bits added to the texture cache key. This also removed the global brightness excess.
- A/B frame-exact (native vs Xenos): combat 4150 31.80 dB, jungle 4140 31.82, 4250 31.65, 3960 31.05, menus 32-33 dB. Remaining differences: 1x vs 4x MSAA edges.
- Polygon offset (PA_SU_POLY_OFFSET_*, read from the PM4 mirror; they are not in the XDK shadow) now applied as D3D12 DepthBias/SlopeScaledDepthBias like Xenia.
- EDRAM same-base aliasing: a color surface bound after a newer same-base surface of another 32bpp format (8888 / 2_10_10_10 / 7e3) receives its bits reinterpreted (src/native/shaders/edram_alias.hlsl). Aliasing checks run only when the RT/DS bind set changes.
- Diagnostics: `--native_skip_ps=<hash>`, `--native_dump_texture_addr=<hex,...>` (DDS), resolve trace shows RB_COPY_DEST_INFO.
- Load freeze after `AudioSystem::SubmitFrame ... dropping frame` is pre-existing (also legacy) - spun off as a separate task.

### EXP-028 - Host MSAA matching the guest sample count
- Scene surfaces are created with the guest sample count (RB_SURFACE_INFO bits 16-17: 0/1/2 -> 1/2/4 samples; cvar `native_msaa`, default on). Samples are part of the surface key; PSOs use the bind set's sample count (MRTs/depth with a different count are dropped with a warning - none seen).
- Color resolves of MSAA surfaces use ResolveSubresourceRegion (AVERAGE), like Xenos' sample-averaging copy. Depth resolves read sample 0 (Texture2DMS variant of depth_copy.hlsl); EDRAM reinterpretation runs per sample (edram_alias.hlsl MSAA variant). Frame dumps skip MSAA surfaces.
- A/B (frame-exact, vs Xenos): combat 4150 31.80 -> 32.05 dB (pixels>32: 0.90 -> 0.73%), jungle 4140 31.82 -> 32.17, 4250 31.65 -> 32.03, 3960 31.05 -> 31.33. Without MSAA 4140 = 31.73.
- Performance (Release, vblank 1000, jungle t50-90): 7.41 ms MSAA vs 7.40 ms without -> free (render thread CPU-bound).
- New diagnostic: hang watchdog (src/native/hang_watchdog.cpp) - no swap for `native_hang_watchdog_s` seconds -> dbghelp stack walk of all threads to the log. The audio/load freeze hit 3 of the first 4 MSAA runs, then 0 of 6 (with and without MSAA), so it looks intermittent rather than caused by MSAA; the watchdog will capture it next time.

### EXP-029 - Render-thread CPU: resolve texture churn, texture write watches, SIMD index scan (7.4 -> 5.57 ms)
- Tooling: the SDK sampler now also records full stacks and the module map; `tools/profile_stacks.py <profile> <slot> <func-substring>` prints callees/leaves/callers of a function. Native renderer draws run on slot 3 (game render thread); slot 0 (main thread) does swap + some resolves.
- Profile (RelWithDebInfo, jungle t50-92): native code = 37% of the render thread. 35% of the thread is the game's own wait (sub_822DDD80 NtWaitForSingleObjectEx).
- Resolve textures were keyed by (dest base, format) only. The game resolves the same memory at different sizes each frame, so the host texture was recreated per resolve (CreateCommittedResource + release = kernel calls, 4.8%), and the command allocator Reset in BeginFrame spent 4.5% in the kernel. The key now includes the size: 16 resolve textures created per run. 7.4 -> 6.0 ms.
- Texture revalidation hashed every texture of <=256 KB every frame (XXH3, 3.4%). Replaced by guest physical write watches (Memory::EnablePhysicalMemoryAccessCallbacks, as used by Xenia's shared memory): per-4 KB-page write sequence, and a texture rehashes only when a page in its range was written after it was armed. Large textures are now also exact (they were checked every 30 frames). Cvar `native_texture_watch`. 6.0 -> 5.7 ms; A/B 32.2-32.3 dB (slightly better).
- Index min/max scan vectorized (SSE4.1 pshufb + min/max_epu16/32, reset index excluded; checked against the scalar version on 200k random cases). 5.7 -> 5.57 ms.
- Remaining native share on the render thread ~27%: PrepareDraw (UploadConstants, SyncRing/Pm4 scan, BindRenderTargets, fixed-function state), GetBuffer partial uploads (barriers + CopyBufferRegion), draw self. Guest-only ceiling (null draws) 3.97 ms, legacy 4.77 ms: beating legacy needs the D3D12 recording moved off the game's render thread (worker + per-draw snapshot).

### EXP-030 - Recording worker thread: native beats legacy (5.57 -> 4.22 ms, legacy 4.77)
- Architecture: the guest threads (render thread draws, main thread swap/resolves) no longer record D3D12. Each hook builds a `WorkCmd` in a batch:
  - The PM4 words written since the last command are copied; the worker parses them with `Pm4Mirror::ScanCopy`.
  - Captured guest ranges: device pointer, device blocks (vertex fetch shadow 0x700, register shadow 0x2880-0x2A50, decl pointer, 0x3080-0x31A0 RT/DS/IB/VB/texture pointers + strides + viewport), RT/DS surface objects, the vertex declaration, resolve rect/point/dest texture/clear color, tiling rects, inline vertex data.
  - Buffer plans: the dirty/clean tracking moved to the guest side (`TrackedBuffer`, `PlanBuffer`, `PlanStreams`). It decides what each draw uploads and copies exactly those bytes.
- Batches of 32 commands go to a worker thread. In the worker, `Load32`/`GuestPtr` read captured ranges first (thread_local capture set per command) and fall back to live memory: texture data (write-watched), LOAD_ALU literal tables, indirect buffers and never-written constant shadows.
- The worker applies buffer plans even for draws it skips, so host buffers stay in sync with the guest-side tracking.
- Direct mode (`native_worker=false`) runs the same captured commands synchronously. First version bug: direct mode parsed the ring at capture time, so the resolve's copy-draw detection (copy_dest_swap) broke; the ring is now always parsed in Execute.
- Swap: the main thread flushes and waits until the worker finished the previous frame (`native_worker_lag`, default on) or the current one (off).
- Results (Release, jungle t50-90, vblank 1000), versus legacy Xenos 4.77 ms (p99 7.3):

  | Configuration | avg | p99 |
  |---|---|---|
  | Worker, lag 0 | 4.49-4.53 ms | 6.4 ms |
  | Worker, lag 1 | 4.21-4.23 ms | 6.2 ms |

  Guest-only ceiling (null draws): 3.97 ms.
- A/B unchanged: jungle 4140 32.40 dB, combat 4150 32.50 dB.
- Profile: render thread native share 27% -> 8% (capture + planning). InvalidateGuestRange (2.6%) now uses stable TrackedBuffer pointers per 64 KB page with a per-call stamp. Worker busy ~34%.
- The intermittent load freeze (see OPEN_QUESTIONS) was caught by the hang watchdog. The worker was idle; every guest thread was in NtWaitForSingleObjectEx. The main thread was in sub_822441F0 -> ... -> sub_826F56C0 -> sub_827035B8 -> sub_826F9000 -> sub_826FA248 -> sub_826F9A20 -> sub_822DDD80 (wait). Log: artifacts/logs/freeze_watchdog_*.log.

### EXP-031 - Fidelity: PWL gamma, alpha to coverage, rectangle-list diagonal (A/B 32.4 -> 34.1/34.2 dB)
- Residual analysis (mean signed difference per region): sand 5-6 levels darker natively (a systematic tint), plus edges, foliage and the distant background (native sharper).
- Gamma textures: the Xenos backend (Xenia) converts GAMMA-signed fetches with the Xenos piecewise-linear curve after filtering, not with sRGB. Now the renderer sets bit 31 of the SRV index in the shared constants for gamma fetches. XenosRecomp (CONAN_RECOMP) masks the index and applies `conanPwlGammaToLinear` (= xenos::PWLGammaToLinear) to .rgb in tfetch2D/3D/Cube. `native_texture_gamma` = 0 raw / 1 sRGB views / 2 PWL (default). Jungle 32.40 -> 33.74, combat 32.50 -> 33.73 dB; the sand tint is gone (bias ~0).
- Alpha to mask (RB_COLORCONTROL bit 4) -> PSO AlphaToCoverageEnable on MSAA targets (foliage edges): 33.74 -> 33.95 / 33.73 -> 33.82.
- Distant blur: the game's DepthOfField pass has four steps. #1139 draws a rect-list quad at the focus depth (0.9648) with z LEQUAL and stencil REPLACE. #1140 writes CoC = saturate(c160.z * saturate(c160.y * (depth - c160.x))) into alpha where stencil EQUAL. Two passes then blur by CoC. Native CoC covered only a triangle, because ExpandRectList always built the 4th corner as v1 + v2 - v0. That corner (3,1) was off-screen for this quad, so the right angle was at v1. Now Xenia's rule: the longest edge is the diagonal, v3 = d1 + d2 - corner. CoC 17k -> 110k pixels (Xenos 114k); distant sharpness now equal; jungle 34.07, combat 34.22 dB. This affected every rect-list post-process quad.
- Bug fixed: with the worker, PassScope still set g_current_pass instead of g_guest_pass, so every command reported pass 0 (per-pass stats, skip/dump debug by pass).
- Tooling: the trace now logs per draw the mirror fetch constants per slot with the bound host resource, the viewport/Z range/VTE, stencil ref/mask, PSO depth/stencil/poly offset, VS/PS c160-163, and inline/rect vertices. `native_debug_vb_addr` logs plans for one vertex buffer. The shader catalog records per-instruction tfetch filter overrides (`artifacts/shaders/dxil/tfetch_filters.txt`, 21 entries, 15 force point, 4 slot conflicts; not applied yet).

### EXP-032 - Xenos pixel centers: A/B 34 -> 50 dB (practically pixel-exact)
- The remaining difference was one-sided bright outlines on every edge, the signature of a sub-pixel shift. Xenos with PA_SU_VTX_CNTL.pix_center = 0 (D3D9 integer pixel centers) is emulated by Xenia by moving the viewport +0.5 pixel (draw_util.cpp, `half_pixel_offset`). The XenosRecomp VS already adds `g_HalfPixelOffset * oPos.w`; the renderer used to pass 0. It now passes (1/w, -1/h) for the draw's viewport (the target size for screen-space draws). Cvar `native_half_pixel_offset`.
- Frame-exact A/B against Xenos:

  | Scene | PSNR | mean abs diff |
  |---|---|---|
  | jungle 4140 | 50.04 dB | 0.25 |
  | jungle 3960 | 51.12 dB | |
  | jungle 4250 | 50.10 dB | |
  | combat 4150 | 50.15 dB | |
  | menus 1500 / 2500 | 58.1 / 57.7 dB | |

  Combat 4150: 0.49% of pixels differ by more than 2 levels, 482 pixels by more than 16 (isolated: hardware alpha-to-coverage patterns, shadow PCF dither).
- Performance unchanged: 4.23-4.24 ms (legacy 4.77).

### EXP-033 - Coverage: new game (prologue level, cinematic, HUD); SRV descriptor corruption fixed
- New tools: `bench/ab_multi.sh <name> <scenario> <exit_s> <swap,swap,...>` compares several swaps in one run. The native side uses `native_ab_swaps` (output-only dumps) and Xenos uses `bench_screenshot_swaps`; `tools/ab_multi.py` prints PSNR per swap and writes a contact sheet. `bench/scenario_newgame.txt` covers title -> Partida nueva -> prologue level (dungeon) with an in-engine cinematic, then walking and combat with HUD and tutorials.
- First results: menus 58 dB, the cinematic 47-52 dB, but gameplay only 31-35 dB. The health-bar frame (quad-list draw, ScreenVert.fxc PS 9849B979) was drawn at full length; Xenos cuts it with an alpha mask (k_8 atlas, `clip(g_AlphaMaskRef - mask)`). Evidence chain:
  - ref overrides: -1 discards everything, 0.5 discards nothing, so the mask read as ~0.
  - The atlas's guest memory never changed after creation, and the D3D12 resource read back at the swap held the correct atlas.
  - `native_texture_watch=false` made it correct, which pointed at SRV allocation patterns.
- Root cause: EndFrameAndPresent allocated an SRV every frame for the blit, then kept its per-frame slot and gave the allocation back with `--srv_heap_next_`. When the allocation came from the retired list (FMV frame textures are recreated every frame, so the retired list is non-empty), the decrement freed the heap top, which belonged to a live texture. The next texture created then overwrote that descriptor. Any texture could show another texture's data after a video. Fix: the blit slots are allocated once.
- Also in this experiment:
  - Texture watch: arm -> hash -> decode, instead of decode -> arm. It closes a race with streaming loads while the worker decodes. It was not the cause here but is a real window.
  - Guest scissor applied (draw_util::GetScissor: window scissor + window offset, clamped by the screen scissor; skipped during predicated tiling). Values match the targets.
- A/B after the fixes: new game 4000 (cinematic) 47.6 dB, 8000/12000/16000 (gameplay with HUD) 50.1-50.8 dB; jungle 4140 50.3 dB.

### EXP-034 - Level-load hang = audio client unregistered with its callback in flight (fixed, conan-port/docs/error_log.md E036)
- Tooling:
  - `bench/repro_freeze.sh <runs> <scenario> [cvars]` classifies runs as clean or HANG using the hang watchdog. Needs the native hooks, which are built in; they beat on every guest swap even in legacy mode.
  - AudioSystem Register/Unregister/Submit-drop logs now record the thread and the client whose callback the worker is executing.
- Before the fix, 5 runs gave 1 hang. That run (conan_273) had `UnregisterClient 0 ... (worker in callback of 0)`, then the E034 drop from the Audio Worker thread, and the ~90 ms later `RegisterClient 0` never came. In clean runs the worker was idle at unregister.
- Guest analysis:
  - Render callback 0x827147C8 -> sub_826FA060: engine CS; `engine+300` = thread; set event A; wait B; submit; `engine+300 = 0`.
  - Mixer thread sub_826F9EE8: wait A; `engine+300 != 0` means a frame tick, 0 means exit.
  - Shutdown on the main thread (sub_826FA248 -> sub_826F9A20): unregister, set A, wait for the mixer thread to exit.
  - A callback still in flight after unregister made the mixer treat the shutdown wake as a tick. It then waited forever, and so did the main thread.
- Fix (SDK src/audio/audio_system.cpp): UnregisterClient waits (condition variable, 5 s warning cap) until no callback of that client is in flight. The in-flight mark is taken under the global lock together with the callback read, and the wait is skipped when called from the worker.
- Verification:

  | Configuration | Runs | Hangs | Notes |
  |---|---|---|---|
  | legacy scenario_combat | 10 consecutive | 0 | run 8 went through the exact race (unregister with callback 0 in flight, one dropped frame); unregister now waits and the re-register follows ~90 ms later |
  | forced race (`audio_debug_callback_delay_ms=20`), `audio_unregister_wait=false` (old behavior) | 2 | 2 | |
  | forced race, with the fix | 2 | 0 | race hit, re-register ok |
  | native renderer A/B mode scenario_combat | 5 | 0 | |
  | legacy scenario_jungle | 5 | 0 | |

  In total, 20 consecutive natural runs without a hang (previously ~1 in 3-6) and no 5 s-cap warnings. Logs: artifacts/logs/freeze_fix_*.txt.

### EXP-035 - In-game pause and menus broken natively = D3DDevice_Clear never reached the native renderer
- User report: levels 1 and 2 render correctly, but once in-game the pause menu and other menus break. Reproduced with `bench/scenario_pause.txt` (load save, START, navigate) through `bench/ab_multi.sh`: 9.8 dB. In native, the pause background (Anark 3D UI, clouds/temple) was missing and menu elements accumulated frame after frame over the frozen scene.
- Evidence chain:
  - Forcing buffer re-uploads (`native_debug_buffers_always_dirty`) or disabling texture watches changed nothing.
  - `native_debug_no_ztest=true` gave 51 dB, so the background quad failed the depth test.
  - Trace: background draw #0 (VS D98E0E92, Anark.fxc) has depthctl 0x232 (z LEQUAL) against the UI depth surface 404FFD80 (1280x720 at EDRAM base 0). WVP gives z = 464.95/474.02 = 0.981, while the native readback of that depth was 0 everywhere: the gameplay upscale pass (p25, depthctl 0x236) writes z = 0 there each frame.
  - The trace showed no clear at all in the pause frame.
- Root cause: the game starts each menu frame with D3DDevice_Clear = sub_822F9EE0(dev, Count, pRects, Flags, D3DCOLOR, f1 Z, Stencil). The XDK implements it (and BeginTiling's clear) with an internal draw, sub_822F9DB8 -> sub_822F9728, built from PM4 templates. It never passes through the hooked draw entry points, so natively neither color nor depth was cleared.
- Fix: hook sub_822F9EE0 (after the original) -> `Renderer::Clear` (captured command, worker `ExecClear`). It clears RT i for flag bit i together with same-EDRAM-base aliases, and the depth-stencil (RT0-size aware), honoring the D3DRECT list. XDK flag values: TARGET0..3 = bits 0-3, ZBUFFER 0x10, STENCIL 0x20; the game uses 0xF and 0x3F. The D3D9 values (1/2/4) would wrongly have cleared depth on color-only clears.
- A/B after the fix:

  | Scene | PSNR |
  |---|---|
  | pause menu, Options submenu, Load Game list, navigation | 48.3-51.1 dB |
  | gameplay after resume | 44.9-47.5 dB |
  | jungle 4140 | 51.1 dB |
  | new game 4000 / 12000 | 47.1 / 50.9 dB |
- Also: the audio unregister race (E036) occurred during a reload from the pause menu and recovered as designed.
- New debug cvars: `native_debug_no_ztest`, `native_debug_buffers_always_dirty`, `native_debug_clears`. Trace additions: VS c160-c167; mirror RB_SURFACE_INFO/COLOR_INFO/DEPTH_INFO/MODECONTROL per draw.

### EXP-036 - Fully native build: Xenos GPU plugin removed from the native path
- Goal (user): nothing from Xenos/Xenia GPU emulation in the native build.
- Design: `conan::native::NativeGraphicsSystem` (conan-port/src/native/native_graphics_system.*) implements `IGraphicsSystem` and is installed from `ConanApp::OnPreSetup` via `config.graphics`, so the runtime never loads the `rexgpu-xenos` plugin. It provides:
  - the D3D12 provider/presenter the native renderer draws into;
  - GPU MMIO registers (0x7FC80000) with the CP_RB_WPTR kick;
  - a sync-only PM4 consumer: read-pointer write-back, fences (EVENT_WRITE_SHD/EXT, MEM_WRITE, REG_TO_MEM, COND_WRITE, scratch write-back), WAIT_REG_MEM, INTERRUPT, XE_SWAP counting, DC_LUT gamma and bin predication. Draw, shader, texture and resolve packets are skipped; the native renderer already has that state from its own PM4 mirror;
  - the vblank thread and the guest interrupt callback.
- SDK change: the `vsync` / `unlocked_vblank_rate` cvars moved from the plugin to rexruntime (`include/rex/system/graphics_flags.h`) so they exist without the plugin.
- Bug found on the first run: the main thread spun in BlockUntilIdle -> BlockOnFence (sub_822DF1D8). That loop waits until `*(dev+10896) >= fence`, where the pointer targets the fence dword the GPU writes with EVENT_WRITE_SHD (address 1FC9C006 = 1FC9C004 with endian k8in32). StoreMemory/LoadMemory applied GpuSwap *and* a byteswap, so the fence landed byte-reversed. Fix: follow Xenia and store GpuSwap(value) in host order. The read-pointer and scratch write-backs are explicitly big-endian (store_and_swap).
- Validation (RelWithDebInfo, `--native_renderer=true`, no plugin; "native graphics: D3D12 presentation ready (no GPU plugin)" in the log):
  - The jungle, newgame, fmv and pause scenarios reach every stage with 0 watchdog hangs, and so do resume, combat and long (10 min, 71.8k swaps). A plain Release launch with no flags now uses this path by default (cp_busy 0).
  - Frame dumps are visually correct: title, main menu, FMV logos, prologue dungeon with HUD and tutorials, jungle gameplay, pause menu, Options, Load Game list, and gameplay after resume (artifacts/captures/ng_sheet.png, ng_pause_sheet.png, nogpu_j_sheet.png).
- Performance (Release, jungle t50-90, novsync, 1000 Hz vblank):
  - native GS 4.23 ms (p99 5.97) vs plugin sync-only 4.22 ms (p99 5.96). The game is guest-CPU-bound (ceiling 3.97 ms), so frame time does not change.
  - The Xenos CP thread (0.96 ms busy/frame) and the plugin DLL are gone.
- `--native_graphics_system=false` brings back plugin sync-only mode. `native_ab_mode` always loads Xenos as its reference. Debug cvar: `native_gpu_trace` (PM4/register log).

### EXP-037 - Alpha release: conan.cfg + launcher, internal resolution, shadow quality, FPS limit, VSync, portable folder
- User goal: first alpha for self-testing. A settings launcher replaces the Xenia-style TOML. Every option must work. Output is a clean, optimized, portable Release folder.
- **Settings** (`conan-port/src/settings.*`): `conan.cfg` next to the exe (INI). `Settings::Apply` pushes it into cvars; command-line flags win.
  - The SDK TOML path moved to the optional `conan_advanced.toml`.
  - Defaults are the original game: 720p, shadows 1024, anisotropy/MSAA as the game sets them, 30 FPS, VSync on, fullscreen, language `auto`.
- **Launcher** (`launcher_dialog.cpp`): Win32, DPI-aware, themed.
  - UI language follows Windows (EN/ES/FR/DE/IT, English otherwise). `--launcher_ui_language=xx` forces one for testing.
  - Options: display mode, window size, FPS limit, VSync, render resolution, shadow quality, anisotropic filtering, MSAA, language, "show at startup" (SHIFT at startup forces the launcher).
  - Buttons: Defaults / Exit / Play; Exit or closing the window quits.
  - Automated runs pass `--show_launcher=false`.
- **Internal resolution** (`render_scale` 1-4): host surfaces are guest size x scale. Viewport, scissor, resolve boxes and destinations, clears, EDRAM reinterpretation, depth copies and the output target are scaled.
  - `g_PixelPosScale` (shared c28.y, new in the XenosRecomp patch) maps the pixel-position input back to guest pixels. Without it, deferred lighting only covered the top-left quadrant at 2x.
  - Reduction targets (<16 px, the 1x576 / 1x1 luminance chain) stay 1:1. Scaling them broke auto-exposure (black image).
- **Shadow quality** (`shadow_quality` 1-3): square targets created in "Render Shadow Maps" use their own scale. The shape test is needed because the scene depth is first touched (cleared) in that pass. The 4096x2048 atlas becomes 8192x4096 at 2x.
- **MSAA** (`msaa_samples`): overrides the sample count only for the surfaces the game multisamples. Surface key fix: the sample count is now log2 (8 << 12 collided with the depth bit).
- **Anisotropic filtering** (`anisotropic_filtering`): applies to linear-filtered samplers; point samplers stay point.
- **FPS limit** (`fps_limit`): the game presents every second vblank, so the native vblank = 2 x fps. Measured 33.4 / 16.67 / 8.33 ms at 30 / 60 / 120.
- **VSync** (`vsync`): the D3D12 presenter now uses Present(1) without tearing (it was always 0 + tearing).
- SDK changes:
  - `window_width/height` no longer change the guest video mode; the guest stays 1280x720 and the window only scales the image.
  - Logging "off" no longer creates empty `logs/` files.
- **Embedded shaders**: the 591 DXIL shaders are packed (`tools/shaders/pack_shaders.py`, CMake custom command) into RCDATA 2 in conan.exe.
  - Bug found by the user: black screen, because the loader read the 20-byte table entries as a 24-byte aligned struct and found no shader. Fixed with memcpy reads.
- **Release** (`tools/make_release.sh` -> `release/Conan-Native-Alpha1/`, git-ignored). Contents: conan.exe (71 MB), rexruntime.dll, app-local VC++ runtime, conan.cfg and `data/` (mirror of conan-port/game, 5.2 GB, without the dangling `shaders/Shaders` symlink).
  - `data/` is discovered next to the exe. Saves stay in Documents\conan.
  - Verified by running the scenario from the release folder: correct frames and no extra files created.
- Validation:
  - 2x resolution + 2x shadows: jungle, FMV, pause menus (also with 8x MSAA + 3x shadows), and new game at 3x (4K), all visually correct with no hangs.
  - Scale-1 A/B vs Xenos: 54.6 / 48.2 dB (unchanged).
- Bench trap: passing the same flag twice makes the command-line parser drop *every* flag. That made `--show_launcher=false` ineffective and runs waited in the launcher. `bench/run.sh` now adds its reproducible defaults (scale 1, fps_limit 0, vsync off, ...) only when the caller did not pass them.

### EXP-038 - Doubled shadow edges above the original shadow quality / render resolution
- User report: with shadow quality above the original (easiest to see with a higher render resolution), shadows show a "double" border. Reproduced in the jungle save: the post's shadow is duplicated at shadow_quality=2.
- Cause: the shadow receivers (e.g. PS 015BD38E30D66637) do manual PCF over `g_ShadowMapTextureAtlas`. Four tap centers are offset by guest-texel constants; each center does a 2x2 bilinear built from point fetches at +-0.5 texel (tfetch offsets) and `getWeights2D`.
  - The translated tfetch offsets and getWeights used the *host* texture size. On a scaled atlas each bilinear footprint shrank by the scale while the tap spacing stayed in guest texels, so the four taps separated into visible copies.
  - Blur kernels over scaled screen-space resolve targets are affected the same way under render_scale.
- Fix: bits 29-30 of the descriptor index carry (scale - 1) for resolve textures rendered at render_scale or shadow_quality. XenosRecomp `TEX_SCALE` / `getGuestTexture2DDimensions` keep tfetch offsets, getWeights2D and bicubic fetches in guest texels. Samples still read the higher-resolution content.
- Validation (swap-exact A/B against Xenos at original quality, same frame):
  - shadow_quality=2: 41.6 dB, same shadow shapes and softness, sharper.
  - render_scale=2 + shadow_quality=2: 35.2 dB (resolution difference only), no doubling.

### EXP-039 - Frame rate cap with VSync off, power draw (handheld PCs)
- User report: the FPS cap does not work with VSync off (it only looked right with VSync forced in the NVIDIA panel), and power draw on a handheld stays high even with a cap.
- Measurement (jungle, fps_limit=30, VSync off). Tools: `tools/power_probe.py` (process CPU + nvidia-smi), `tools/thread_cpu.ps1` (per-thread CPU), and a presents/s counter in the D3D12 presenter (info log).
  - The guest ran at exactly 33.3 ms, but the presenter did **~300 presents/s**. The SDK's AchievementToastDialog is always registered; any registered ImGui dialog made the UI thread repaint continuously, and the DXGI UI tick did not bound it. That is what FPS counters and the GPU saw.
  - The process used 1.41 cores. The guest swap thread alone took 84% of a core, spinning in the XDK BlockOnFence (sub_822DF1D8) -> sub_822DE050 poll: the CPU busy-waits on the fence while the GPU waits for the vblank that paces the frame. The PM4 consumer also spun (MaybeYield) in WAIT_REG_MEM.
- Fixes:
  - SDK: `ImGuiDialog::WantsContinuousRepaint()` (the toast only while a toast is queued); ImGuiDrawer requests continuous repaints only when a dialog wants them.
  - Conan (user request): no SDK overlays at all. The F3/Backtick/F4/F7 binds are unregistered and there is no achievement toast.
  - Native GS: a GPU progress generation + condition variable, signaled on every memory store (fences) and read-pointer write-back. A hook on sub_822DE050 records the generation before the poll and, when the poll says "keep waiting", sleeps until progress (1 ms cap, so the XDK hang detector still runs).
  - Native GS: WAIT_REG_MEM on registers blocks on an event set by vblank and MMIO writes; memory polls sleep 100 us.
- Results (Release unless noted):

  | fps_limit | frame time | presents/s | process CPU | GPU power |
  |---|---|---|---|---|
  | 30 (before, nr) | 33.33 ms | ~300 | 141% of a core | 25.9 W |
  | 30 (after, nr) | 33.33 ms | 30 | 57% | 21.8 W |
  | 60 | 16.67 ms | | 98% | 27.1 W |
  | 120 | 8.33 ms | | 196% | 42.7 W |

  - Unlocked (1000 Hz vblank): 4.12 ms (was 4.23).
  - CPU now scales with the frame rate instead of spinning.

### EXP-040 - CPU/GPU cost audit at a frame rate limit (idle spins, GPU time per pass)
- Goal (user): CPU and GPU use still look high for a 2007 game on a 5800X3D + RTX 4080. Find waste and cut it.
- New tools:
  - `tools/thread_cpu.py`: per-thread CPU with thread names.
  - `--sample_profile_all_threads`: the SDK sampler covers every thread.
  - `tools/profile_threads.py`: "active" (non-OS-wait) leaves per thread.
  - Native GPU timestamps (`native: GPU time` log every 5 s); `--native_gpu_pass_timing` splits them per render pass.
- Waste found and fixed (fps_limit=30):
  - **SDK TimerQueue** spun in `disruptorplus::spin_wait_strategy` (inherited from Xenia): 17.6% of a core while idle. It now uses `blocking_wait_strategy`. Two fixes were needed: the vendored header passed the `wait_until` arguments in the wrong order, and an empty queue waits 1 s instead of `time_point::max()` (which overflows).
  - **Guest db16cyc busy-waits**: every loop whose body is only `db16cyc` (a Xenon priority hint that compiles to nothing): the engine job worker (sub_824F0E60), the render thread's job submit/wait (sub_824F1110, sub_824F2520, sub_824F2470), sub_824D5618 and sub_822F33C0.
    - Mid-asm hooks call `conan_spin_idle_wait`: pause for the first 2 ms of a wait, then 250 us high-resolution sleeps. Only the idle time between frames sleeps.
    - Thresholds of 100 us / 500 us made an unlocked frame 25.6 / 8.8 ms (vs 4.2): the render thread <-> worker handoffs inside a frame routinely exceed 500 us.
  - **BlockOnFence**: a thin wrapper on sub_822DF1D8 records (device, fence). The sub_822DE050 poll hook then waits for that exact fence condition, instead of waking on any GPU progress.
  - **PM4 sync consumer**: registers >= 0x2000 (render state, constants) use a relaxed-store fast path (was a locked xchg plus a cvar check per register). Its thread went from 4.5% to ~2.5%.
  - GPU hang breadcrumbs (a MARKER_OUT WriteBufferImmediate after every draw) are now opt-in (`native_gpu_breadcrumbs`). No difference on NVIDIA; pipeline-draining markers tend to cost more on AMD.
- Results (Release, jungle):

  | | process CPU @30 | process CPU @60 | unlocked frame |
  |---|---|---|---|
  | start of EXP-039 | 1.41 cores | - | 4.23 ms |
  | after EXP-039 | 0.57 | 0.98 | 4.12 ms |
  | after EXP-040 | 0.45-0.49 | 0.79-0.87 | 4.23-4.31 ms |

- GPU:
  - 1.3 ms/frame at full clocks. Render Opaque (p12, 1024x576 4xMSAA HDR foliage) is ~0.6 ms; every other pass is <= 0.14 ms.
  - At 30 fps the timestamps read ~6-7 ms because the driver drops clocks. GPU board power 21-22 W, the idle level of this card.
- What remains is mostly real work, per frame at 30 fps: game logic and render threads ~5-6 ms, the D3D12 recording worker ~2-3 ms, the PM4 sync consumer ~0.8 ms, audio and misc.
- Not changed: the HDR scene RT format (R16G16B16A16_FLOAT for Xenos 7e3). R11G11B10 would halve bandwidth but drops the alpha that blending and the EDRAM reinterpretation use.

### EXP-041 - Pixelated/serrated shadow edges at shadow_quality > 1
- User report: after EXP-038 the high-resolution shadows no longer double, but their edges show a pixelated, patterned look.
- Cause: EXP-038 kept the PCF kernel in guest (1024-map) texels on a map with 2-3x more texels. Each point tap then picks one sub-texel of a guest texel, so the bilinear weights (guest grid) and the fetched texels (host grid) disagree: serrated, aliased edges.
- The receivers of `g_ShadowMapTextureAtlas` (303 shaders) bake their rotated-grid PCF tap-center offsets as shader literals in 4096-texel atlas units: {-0.5, 0.75, -1.5, -0.25}/4096 and {1.5, 0.25, 0.5, -0.75}/4096 (c254/c255, or c253/c254 in 49 shaders). Each center is a 2x2 bilinear built from point fetches at +-0.5 texel plus getWeights2D.
- Fix (same idea as re:Blue's g_ShadowPcfScale):
  - XenosRecomp (CONAN_RECOMP): in pixel shaders that sample g_ShadowMapTextureAtlas, literal vectors whose components are all multiples of 0.25/4096 within +-2/4096 are multiplied by `g_ShadowAtlasTexelScale` (shared c28.z). This matches exactly the 303 intended pairs and nothing else.
  - Host: g_ShadowAtlasTexelScale = 1 / shadow_quality. Shadow-map resolve textures no longer carry the TEX_SCALE bits, so their bilinear taps and weights use the host texel grid.
  - The whole kernel now runs at the higher resolution with its original shape: consistent, no pattern, sharper penumbra. Identity at shadow_quality 1.
  - `--native_shadow_pcf_mode=1` restores the EXP-038 behavior for comparison.
- Validation: static camera in the jungle (swap 6200, render_scale 2), shadow 1x vs EXP-038 vs the new mode.
  - The character's shadow edge is serrated with EXP-038 and smooth with the new mode.
  - The leaf dapples on the ground look the same in all three (blocky in the original data).
- Note: the p9 "decal" buffer 1CA78000 bound as g_ParallelLightShadowTexture/slot 2 in some draws is the SpeedTree leaf/branch color layer (samCompositeLeafMap), not a shadow mask. The jungle's sun-like light is a spot light with a leaf cookie plus the atlas.

### EXP-042 - Shadow quality Ultra = 4096, MSAA removed from the settings
- User request: Ultra shadows should be 4096, and the MSAA option makes no sense (the game is designed around 4x). Remove it; always 4x.
- `shadow_quality` is now 1 / 2 / 4 (1024 / 2048 / 4096; old cfg values snap to the nearest). Ultra makes the atlas 16384x8192 R32F (the D3D12 maximum, 512 MB).
- The raw D24S8-as-RGBA8 copy that depth resolves also produce (for k_8_8_8_8 fetches) is no longer created for shadow-map resolves. Nothing reads it, and it would add another 512 MB at Ultra.
- MSAA is gone from the launcher and conan.cfg. `msaa_samples` stays as a debug cvar (0 = the game's 4x).
- Verified: jungle at render_scale 2 + shadow_quality 4 renders with no errors, and shadow edges are the sharpest of the three modes. Launcher shows 3 graphics rows.
- tools/capture_window.py now only touches the window of the process it starts. The previous version found the first ConanLauncher window on the desktop, which could be the user's own open launcher, and sent it WM_CLOSE.

### EXP-043 - Arbitrary internal render resolutions (not only 720p multiples)
- User question: can arbitrary resolutions be supported simply? Yes; the host surfaces already separated guest size from host size.
- `render_scale` is now a double (1.0-4.0).
  - Surface scale is a float. Every guest -> host coordinate (surface and resolve-texture sizes, resolve boxes and destinations, clear rects, scissor, EDRAM reinterpretation) goes through one rounding function (`HostPx`, lround). Adjacent tiles and atlas cells still meet exactly.
  - The viewport is scaled in float. The output target is HostPx(1280) x HostPx(720).
- Descriptor index encoding: TEX_SCALE moved from bits 29-30 (integer 1-4) to bits 16-27 (8.8 fixed point, 0 = 1:1). SRV indices use bits 0-14; bit 31 stays PWL gamma.
- Settings:
  - `RenderResolution` in conan.cfg is now a 16:9 render height (720-2880), or 0 = the primary display's native height (physical mode via EnumDisplaySettings, fitted to 16:9).
  - Old values 1-4 convert to 720 x value.
  - Launcher: Native display / 720p (original) / 900p / 1080p / 1440p / 1800p / 2160p / 2880p. A custom height typed into the cfg shows as an extra entry.
- Validation (jungle, static camera swap 6200):
  - render_scale 1.5 (1920x1080) and 1.111 (1422x800) render correctly with no seam at the tile boundary. The centre-column difference, 2.2-2.3, is within the image's normal 1.7-1.9.
  - Scale 1 A/B vs Xenos: 46.4 dB.
  - "Native" resolved to 1440 on the 2560x1440 monitor.
- Safety: bench/run_safe.sh now kills only conan.exe processes started from its EXE_DIR, never a game the user has open. tools/capture_window.py already only touches its own process (EXP-042).

### EXP-044 - Shader-compilation stutter: PSO precompilation cache
- Measurement: new game scenario; native PSO creation timed and logged ("PSO #n created in x ms").
  - The native shaders are offline DXIL. The stutter is the driver compiling each new PSO (DXIL -> ISA) on first use.
  - Warm driver shader cache: 121 PSOs, median 0.05 ms, max 0.94 ms, no stutter.
  - Cold driver cache (first run, new driver, or a new shader corpus/release): 121 PSOs, 5.2 s of synchronous compilation on the worker. The heavy lighting pixel shaders take ~190-270 ms each.
    - Spikes: a 4.2 s freeze plus 200-500 ms hitches.
    - A cold cache was simulated with `--native_debug_rs_salt=N` (an unused root-constant block changes every PSO for the driver). `ShaderCacheControl` needs Windows developer mode (887A002D).
- Fix: `PipelineCache` (conan-port/src/native/pipeline_cache.*).
  - Every PSO the renderer creates is recorded as a pointer-free `PsoRecord` (full D3D12 state + shader hashes + input layout by usage code), keyed by the renderer's pipeline key, and appended to `conan_pipelines.bin` next to the exe.
  - At renderer init, the base set embedded in conan.exe (RCDATA 3 = artifacts/shaders/pipelines_base.bin, from tools/make_pipeline_base.sh) plus the local file are created on 1-4 below-normal-priority threads, in first-seen order, while logos and intros play.
  - `GetPipeline` takes a precompiled PSO, or waits for one in flight (never slower than compiling it again). A known PSO the workers have not reached yet is compiled synchronously and marked done.
  - Shaders for the workers come straight from the embedded DXIL pack (thread-safe, no copies).
  - `--native_pipeline_cache=false` disables it.
- Results (new game, fps_limit 60):

  | | PSOs compiled during play | >100 ms frames | worst frame |
  |---|---|---|---|
  | cold, no precompile | 121 (5.2 s) | 8 | 4209 ms |
  | cold, precompile | 0 (121 precompiled in 1.6 s at startup) | 4 | 214 ms |
  | warm (driver cache), precompile | 0 | 4 | 204 ms |

  - The 4 remaining spikes (11, 13.4, 29.8, 32.5 s) are identical warm or cold: title/menu and level-load transitions, not shaders.
- Coverage: PSOs never seen in a captured playthrough are compiled on first use (first run only). They are then recorded and precompiled on every later start. A conan_pipelines.bin from a full playthrough can be folded into the embedded base (records are machine-independent).

### EXP-045 - Graphics enhancements (launcher "Enhancements" group; all off = original look)
All are generic (no per-scene tuning). Each is a conan.cfg key, a cvar and a launcher control.
- **3D scene at full resolution** (`full_scene_resolution`): the game renders its scene at 1024x576 and stretches it to 1280x720 (pass Upscale). Every non-output-sized surface gets an extra 1280/1024 scale, so the scene reaches the output 1:1 (sharper; the upscale pass becomes a 1:1 copy).
- **Foliage antialiasing** (`foliage_antialiasing`): opaque alpha-tested draws (func GREATER/GEQUAL, no blending) into a multisampled target use alpha-to-coverage instead of the hard alpha test.
  - Shader: the UnleashedRecomp SPEC_CONSTANT_ALPHA_TO_COVERAGE branch, enabled for CONAN_RECOMP: mip-corrected alpha (computeMipLevel), then `0.5 + (a - ref) / fwidth(a)`.
  - About 145 draws/frame are converted (passes 8, 9, 12, 13). Leaves are denser and antialiased with the game's 4x MSAA.
- **FXAA** (`fxaa`): an FXAA 3.11-style pass (shaders/fxaa.hlsl) on the Upscale target at the first HUD-pass command. The HUD stays sharp.
- **Ambient occlusion** (`ambient_occlusion`, `ssao_radius` 0.5, `ssao_intensity` 0.8, `ssao_fade_distance` 40):
  - The camera comes from the game's own `g_mProjectionToWorld` (inverse view-projection). tools/shaders/gen_projection_regs.py builds the PS-register table of the 343 shaders that declare it from the catalog reflection, and the matrix is read from the PM4 mirror during Opaque draws. World units are meters (far plane 199).
  - AO: 12-sample normal-oriented hemisphere in world space, world-space occlusion test with range falloff, interleaved-gradient rotation.
  - Apply: depth-aware 4x4 blur, multiplied into the HDR scene (blend dest*src) at the start of End Tiling, i.e. after Opaque/Character/Skybox and before fog, transparents and post.
- **Bloom quality** (`bloom_quality` 1/2/4): small (<=512) post targets get an extra resolution factor.
- **Dithering** (`present_dither`, the SDK presenter's dithered output).
- **Shadow smoothing** (`shadow_smoothing` 0/1/2): in the atlas receivers, XenosRecomp emits a per-pixel rotation (interleaved gradient noise on floor(iPos)) of the baked PCF tap-center offsets, widened by `g_ShadowSoftness` (shared c28.w: 0 / 1.0 / 1.6).
  - The 2x2-bilinear staircase along shadow-map texels becomes a stable fine grain.
  - Mode 1 removes the stair-stepping; mode 2 gives a wider penumbra.
- **Soft particles** (`soft_particles`, `soft_particle_distance` 0.4):
  - Which draws: blended, depth-tested, non-depth-writing draws of Render Sorted fade out near the scene surface behind them.
  - Data used: the frame's scene depth resolve (R32F), and the view depth from the w column of the inverse view-projection (`1/w = z*M2.w + M3.w`).
  - Fade mode: rgba for premultiplied/additive blends, alpha only for SRC_ALPHA (spec bits 4/5).
- Launcher: two columns (Display + Graphics + Game | Enhancements), 864x481 at 100%, so it fits 1280x800 at 150%.
- Cost on the RTX 4080 at 1080p with everything on: GPU 1.34 -> 2.28 ms/frame (SSAO ~0.47 ms, bloom x4 ~0.24 ms).
- Validation:
  - All options on across FMV, pause/menus, jungle and new game: correct frames, 0 hangs, no errors (one known E036 audio drop).
  - Defaults (scale 1) A/B vs Xenos: 54.6 / 47.3 dB, unchanged.

### EXP-046 - Square/striped patterns on glows and particles above 720p
- User report: the spectres' glowing eyes and particles in the new game show square or linear patterns instead of smooth glows. Seen at render_scale 2, not at 1.
- Isolation: `--native_skip_ps=91D1ACA97DA29607` removes the whole magenta spectre aura/tail/eyes. It is a Render Sorted blended sprite shader sampling one 128x128 BC3 texture (bilinear), so there is no sampling bug.
  - At 720p the scene is 1024x576 stretched, and the sprite used a coarser mip.
  - At 1440p the finest mip is magnified: wisp streaks, bilinear diamond/cross shapes and BC blocks show.
- Fix (`smooth_effects`, default on; acts only when the bound target is above the original resolution): textures < 256x256 in blended draws get
  - cubic B-spline magnification (descriptor-index bit 28 -> tfetch2DBicubic in tfetch2D), and
  - a mip LOD bias of log2(scale): the same texel density as the original, so the same softness.
- Also: point fetches of upscaled color resolve targets (post-process chains) now sample linearly. At the guest texel positions the game samples, that averages exactly the host texels of that guest texel. R32F depth resolves stay point.
- Result: the spectre's tail and aura are smooth like the original at 1440p. The rest of the scene keeps the higher resolution.

### EXP-047 - Optimization round for the beta release
- Measured with `bench/opt_baseline.sh <tag>` (Release):
  - jungle unlocked (t50-90);
  - new game unlocked;
  - CPU/RAM/GPU power at 60 fps;
  - VRAM.
  `REL=<build dir>` selects the build. Results in artifacts/profiles/opt_<tag>.txt.
- Changes:
  - **sub_824E7678 cache** (native_hooks.cpp): the game's per-draw parameter name lookup (a pure function of the object's table pointers + name) is memoized per thread (XXH3 key, clear at 64K entries). Jungle 4.28 -> 4.10 ms.
  - **PM4 consumer** (native_graphics_system.cpp): type-0 register writes and constant loads at or above 0x2000 are skipped (nothing reads them; one-time warning if something does). Removes most of the CP thread's CPU.
  - **Capture arena** (`WorkBatch::bytes`): allocator without value-initialization (`DefaultInitAllocator`), so resizing no longer zero-fills bytes that are overwritten anyway.
  - **Debug cleanup**: 16 `native dbg` log blocks removed from native_renderer.cpp. They were gated by `BenchElapsedMs()` clock queries on per-draw paths. The cvar-gated diagnostics (trace, dumps, `native_debug_*`) stay.
  - **x86-64-v3 (AVX2/FMA/BMI2)** for the game build (conan-port CMakePresets + nr-rel):
    - `-ffp-contract=off` keeps the recompiled PPC float math unfused, so rounding is unchanged. A/B vs Xenos at scale 1: jungle 54.6/51.8/50.4 dB, new game 54.6/51.8/51.0 dB (unchanged).
    - Paired jungle runs: v3 4.08-4.41 ms vs v2 4.48-4.50 ms.
    - `src/cpu_check.cpp` is compiled for baseline x86-64 and runs first (init_priority 101). On a CPU without AVX2 it shows a message (EN/ES) instead of crashing with an illegal instruction.
  - Upload ring: an `upload_peak` KB counter was added to the vram log. The peak is 59 MB (new game load) of 128 MB per frame, with 0 overflow pages. The ring was kept: sysmem 442 MB total, and 6 GB handhelds have ample headroom.
- Result (base -> final):

  | Metric | Base | Final |
  |---|---|---|
  | Jungle unlocked | 4.28 ms (p99 6.13) | 4.03-4.11 ms (p99 5.5) |
  | New game unlocked | 3.08 ms | 3.16 ms (noise) |
  | CPU at 60 fps | 87% of one core | 74-79% |
  | GPU board power at 60 fps | 32.7 W | 25-32 W |
  | RAM working set | 1.55 GB | 1.56 GB |
  | VRAM | 361 MB | 361 MB |

  Hitches: 0 over 50 ms.
- Memory: the footprint (RAM 1.6 GB, VRAM 0.36 GB + 0.44 GB upload heaps) fits a 6 GB/6 GB handheld with room to spare. No texture eviction happens (retired 0), so everything stays resident.

### EXP-048 - Launcher: fixes always on, MSAA option back
- User request: "3D scene at full resolution" and "smooth glows/particles above 720p" are fixes, not enhancements. They are now always on: removed from the launcher and conan.cfg, and forced in Settings::Apply. The `full_scene_resolution` cvar now defaults to true. bench/run.sh still passes `false` so the A/B vs Xenos stays 1:1.
- MSAA is back as an option under Graphics: Off / 4x (original, default) / 8x.
  - cfg key `MSAA` = 1/4/8. The old value 0 (game default) maps to 4.
  - Foliage antialiasing is alpha to coverage, so it needs MSAA: with Off the checkbox is unchecked and greyed out, and Apply also forces it off.
- MSAA check (jungle, 1080p, every enhancement on, native dumps at swaps 3000/4200):
  - All three modes render correctly.
  - vs 4x at swap 3000: Off 30.9 dB (aliased edges), 8x 35.5 dB. The differences are only on geometry and foliage edges.
  - Swap 4200 differs by gameplay divergence only.
  - No errors (the known E036 audio drop only).
- Layout: the Game group moved under Enhancements. The window is now 864x447 at 100% (was 481).
