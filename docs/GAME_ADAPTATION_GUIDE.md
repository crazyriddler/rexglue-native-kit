# Adapting the reference native renderer to a new game

The Conan renderer (`reference/conan/port/src/native/`) is ~90% generic Xbox 360 XDK
knowledge and ~10% game-specific facts. This guide lists every game-specific fact, where
it lives, and how to rediscover it for a new title. Copy the reference sources into
`$PORT_DIR/src/` and change **only** these items (rename `conan` namespaces/strings to the
new game while you are at it).

## 1. Find the XDK D3D functions (automated first)

1. Decoded image: `python scripts/port/xex_decode.py $PORT_DIR/game/default.xex $PORT_DIR/logs/default_image.bin`
   (basic compression only) or, after the first build, run the game with
   `--dump_xex_image=<file>` (any compression; kit SDK feature).
2. Disassembly: `tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc -EB --adjust-vma=0x82000000 default_image.bin > $PORT_DIR/logs/default_full.dis`
3. Signatures: `python tools/re/xdk_sigs.py match $PORT_DIR/logs/default_image.bin $PORT_DIR/generated/default/<game>_register.cpp -o artifacts/xdk_match.tsv`
   - `exact` = all masked instructions equal -> very likely the same XDK revision;
     still confirm the hook targets below in the disassembly.
   - `fuzzy`/`missing` = different XDK revision or LTCG layout: find the function
     semantically (next section) and add it to your own symbol table.
4. Semantic confirmation tools (`tools/re/`, need `CODE_START` env = REX_CODE_BASE):
   `pm4scan.py` (functions that build PM4 packet headers: DRAW_INDX 0xC0012201/0xC0032201
   = draw entry points, VIZ_QUERY, EVENT_WRITE...), `q.py callers/callees/dis`,
   `callargs.py`, `constwriters.py` (device constant writers dev+0x780/0x1780),
   `statetables.py` (render/sampler-state dispatch tables -> every setter by name; update
   its table addresses from a pointer scan), `apisurface.py`, `gamelayer.py`, `ptrtables.py`.

### Hook list (role -> Conan address -> what it does)

| Role | Conan | Reference file | Notes |
|---|---|---|---|
| D3DDevice_DrawVertices | 82580918 | d3d_capture.cpp | (dev, prim, start, count) |
| D3DDevice_DrawIndexedVertices | 82580D00 | d3d_capture.cpp | (dev, prim, base, startIndex, count) |
| D3DDevice_DrawVerticesUP | 825808B8 | d3d_capture.cpp | Begin + memcpy + End |
| D3DDevice_BeginVertices / EndVertices | 825803F8 / 82580898 | d3d_capture.cpp | inline vertices (HUD) |
| D3DDevice_Resolve | 822F5028 | d3d_capture.cpp | (dev, Flags, pSrcRect, pDestTex, pDestPoint, Level, Slice, pClearColor, Z, Stencil, pParams); Flags&7 = RT (4 = depth), 0x100/0x200 clear |
| D3DDevice_BeginTiling / EndTiling | 822F3EF8 / 822F4480 | d3d_capture.cpp | predicated tiling |
| D3DDevice_Clear | 822F9EE0 | d3d_capture.cpp | XDK flags TARGET0-3 bits 0-3, ZBUFFER 0x10, STENCIL 0x20 |
| D3D ring segment switch / large alloc | 822DF848 / 822DF548 | d3d_capture.cpp | PM4 mirror resync |
| Inline SET_CONSTANT reserve | 82580358 | d3d_capture.cpp | |
| Shader literal loader (LOAD_ALU_CONSTANT) | 822F7408 | d3d_capture.cpp | walked table {u16 reg, u16 dwords, u32 offset} |
| GpuBeginShaderConstantF4 | 822E7A48 | d3d_capture.cpp | ring constants |
| VB / IB Unlock | 822EA0D8 / 822EA1E0 | d3d_capture.cpp | dirty tracking |
| CreateShader (x2) | 822E84C0 / 822E85D0 | shader_registry.cpp | r3 = container in, object out |
| D3DDevice_Swap | 822E8EB8 | native_hooks.cpp | frame boundary |
| BlockOnFence + its poll | 822DF1D8 / 822DE050 | native_hooks.cpp | sleep instead of spin |

All of these are XDK library code: for games built with the same XDK revision the
signatures match exactly.

## 2. Game-specific facts to rediscover

| Fact (Conan value) | Where used | How to find it |
|---|---|---|
| Device pointer global `0x82C81A64` (`kDevicePtrAddr`) | native_renderer.cpp | The global written with Direct3D_CreateDevice's ppDevice result; most `lwz` refs in game code |
| Device struct offsets (0x480 fetch, 0x780/0x1780 consts, 0x2780.. bools/loops, 0x2E24 decl, 0x3090 RTs, 0x30A0 DS, 0x30F8 textures, 0x3160 viewport, 0x318C/0x3190 shaders, ring at +0x30/+0x34) | native_renderer.cpp constants, capture ranges | Same XDK revision -> identical. Otherwise confirm from setters (SetTexture, SetRenderTarget, Set*Shader, SetViewportF) - see XDK_D3D_NOTES.md |
| Front buffer global `0x82C81A68` | (Present) | Resolve/Swap argument in the game's present function |
| Output resolution 1280x720 (`kOutputWidth/Height`) | native_renderer.h, kit.env GUEST_WIDTH/HEIGHT | Front buffer size / VdSwap |
| Scene resolution 1024x576 upscaled (`full_scene_resolution` factor 1280/1024) | native_renderer.cpp surface scale | Trace: the main HDR/scene surfaces and the Upscale pass. If the scene is already 1280x720, the factor is 1 |
| Render pass table (29 passes, `PassScope` indices, `kPassRenderShadowMaps=4`, Opaque 12, EndTiling 16, Sorted 21, Upscale 25, HUD 27) | d3d_capture.cpp CONAN_PASS_HOOK list, native_renderer.h | Look for a table of {enabled, name*, fn*} with pass-name strings ("Render Shadow Maps"), or the game's GPU timer/perf-marker calls; `ptrtables.py`, `passtable.py`. Without names: bracket draws by RT changes in a trace |
| Shadow pass detection (square targets created during the shadow pass) | surface scale | Trace the shadow-map surface/atlas resolves |
| Shadow atlas PCF literals (multiples of 0.25/4096) and sampler name `g_ShadowMapTextureAtlas` | XenosRecomp patch | Shader catalog reflection: sampler names of shadow receivers. Other engines: different names/kernels - adapt or disable that patch hunk |
| Camera: `g_mProjectionToWorld` register per shader (`projection_regs.inc`) | SSAO, soft particles | `tools/shaders/gen_projection_regs.py`: change the constant name to your engine's inverse view-projection (or view/projection pair) from the catalog reflection |
| Present function `sub_82533178` | d3d_capture.cpp | Caller of Resolve(front buffer)+Swap |
| Engine hot pure function `sub_824E7678` (name lookup cache) | native_hooks.cpp | Only after profiling; do not port blindly |
| Guest busy-wait loops (`db16cyc`-only bodies): 8 midasm hooks `conan_spin_idle_wait` | conan_manifest.toml, hooks.cpp | Profile at a frame cap; find loops whose body is only `db16cyc` + a load/compare (the instruction compiles to nothing) |
| Crash guards / function boundaries | conan_manifest.toml, hooks.cpp | Per game, reactive (LESSONS_LEARNED B/C) |
| Game presents every 2nd vblank (vblank = 2 x fps) | native_graphics_system.cpp fps_limit | Measure: frame time at a known vblank rate |
| Title ID 545107DA, shader container location (`shaders/shaders.stx`) | kit.env, corpus | Log line "Initializing shader storage for title"; `extract_shaders.py` scans every file |
| Launcher strings/title, window title, cfg name, icon | launcher_dialog.cpp, settings.cpp, app header, .rc | Rename |

## 3. Things that are generic (keep as is)

PM4 mirror, NativeGraphicsSystem, texture decoder, EDRAM surface model, resolve paths,
buffer tracker, PSO cache, worker threading, shared constants + XenosRecomp patch (minus
the Conan heuristics above), settings/launcher framework, cpu_check, hang watchdog,
release tooling. Expect to fix new formats/primitive types/packet types the new game uses
that Conan did not (log unknown ones once, then implement).

## 4. Order of work for a new game (native phase)

1. Hooks from section 1 in **capture-only** mode (call originals, Xenos still renders) +
   `d3d_capture` catalog (draws/resolves/passes per frame).
2. NativeGraphicsSystem later; first run the native renderer in A/B mode next to Xenos
   (`native_ab_mode`) so every step is compared frame-exactly.
3. HUD/2D first (BeginVertices, simple states), then post chain, then scene passes.
4. When A/B >= ~45 dB across scenarios, switch the default to native + NativeGraphicsSystem
   (no plugin) and keep A/B mode for regressions.
