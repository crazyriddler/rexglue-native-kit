# Upstream research map

The only index of external projects and the evidence taken from them. Rules derived from this
evidence live elsewhere (ANY_GAME_CHECKLIST, PERFORMANCE_GUIDE); this file keeps what each
project does, with commits and file paths, so a finding can be re-checked. Clone disposable
copies shallowly into `_research/upstream/<repo>` (gitignored; `scripts/fetch_upstream_references.ps1`
clones the main ones); never mix them into the game's source tree unless code is
intentionally adapted (then note its license).

## Project index

| Project | URL | Use it for | Section |
|---|---|---|---|
| ReXGlue SDK | https://github.com/rexglue/rexglue-sdk | IGraphicsSystem, plugin loading, manifests, codegen, runtime; base of `sdk/` (`c94f5eb`) | throughout |
| XenosRecomp | https://github.com/hedge-dev/XenosRecomp | shader container parsing, ucode -> HLSL -> DXIL | "Native renderer reference architecture" §3 |
| reblue-XenosRecomp | https://github.com/zolaware/reblue-XenosRecomp | the fork the kit's shader pipeline pins (`339af41` + kit patch) | "Native renderer reference architecture" §3 |
| UnleashedRecomp | https://github.com/hedge-dev/UnleashedRecomp | XDK-hook renderer, PSO precompile at load, fps/aspect patches, codegen flags | "Native renderer reference architecture" §1-2; findings also in ANY_GAME_CHECKLIST |
| reblue (Blue Dragon) | https://github.com/zolaware/reblue | ReXGlue 0.10 native renderer on plume: PSO predictor, occlusion, physical buffers, DRED, frame interpolation | "Native renderer reference architecture" §5; "Survey" (reblue) |
| plume | https://github.com/renderbag/plume | RHI used by UnleashedRecomp/reblue/LostOdyssey | "Native renderer reference architecture" §4 |
| skate3recomp | https://github.com/mchughalex/skate3recomp | engine-level renderer, TU codegen, ultrawide frustum patch | "skate3recomp native renderer" |
| The Darkness Recomp | https://github.com/portingpete/The-Darkness-Recomp | engine-level D3D11, XenonRecomp fixes, pacing findings, WARP tests | "The Darkness Recomp" |
| LostOdysseyRecomp | https://github.com/freefrank/LostOdysseyRecomp | measurement discipline, clear coalescing, recompiler audit, UE3 shaders, 3C6T | "LostOdysseyRecomp" |
| VivaPinataRecomp | https://github.com/crabinacrabic/VivaPinataRecomp | ReXGlue 0.10 nightly (Xenos renderer): vpkd3d128 codegen bug, adjustor-thunk and vtable sweeps, manifest pre-flight, language cvars, AGENTS.md install flow | "VivaPinataRecomp" |
| AC6_recomp and 10 more ReXGlue ports | see the survey table | ReXGlue behaviour across games: [rexcrt], setjmp, thread races, TU diffing | "Survey of other ReXGlue ports" |
| dc3-decomp | https://github.com/rjkiv/dc3-decomp | CC0 XDK symbol map -> `tools/re/xdk_2012_dc3_symbols.tsv` | "Survey of other ReXGlue ports" |
| NX1recomp | https://github.com/goshavindtburg/NX1recomp | ReXGlue + XenosRecomp project structure, shader dump tooling (not surveyed yet) | - |
| Xenia | https://github.com/xenia-project/xenia | behavioural oracle for Xenos semantics (packets, EDRAM/resolves, formats, registers), not the target architecture | - |

---

## Native renderer reference architecture (UnleashedRecomp/reblue)

Researched 2026-09-24. Shallow clones live in `_research/upstream/` (gitignored). Commits inspected:

| Repo | Local path | Commit |
|---|---|---|
| hedge-dev/UnleashedRecomp | `_research/upstream/UnleashedRecomp` | `cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c` (2026-06-29) |
| zolaware/reblue (Blue Dragon, NOT Sonic '06) | `_research/upstream/reblue` | `ce0edadbb79007526842dadebf80941bf8ea46ca` (2026-09-19) |
| hedge-dev/XenosRecomp | `_research/upstream/XenosRecomp` | `990d03b28a27b50277ee5d8d942e1c5f873869d1` (2025-08-03) |
| zolaware/reblue-XenosRecomp | `_research/upstream/reblue-XenosRecomp` | `339af41df2c23dbe3256c1c377716b81a0e0fe6b` (2026-08-22) |
| renderbag/plume | `_research/upstream/plume` | `d72379344dacd3dbf9f810f92ddc87e6de1845b1` (2026-09-21); re:Blue pins fork zolaware/plume `e0c8871b` |
| rexglue/rexglue-sdk | `_research/upstream/rexglue-sdk` | `main` = `c94f5eb` (= our v0.10.0 base); `development` = `923c1a5` also fetched |

### 0. Key conclusion

**re:Blue is the closest template for this project.** It is a ReXGlue **v0.10.0** port (`reblue_manifest.toml: sdk_version = "0.10.0"`), same SDK as ours. It turns the SDK's Xenos GPU off (`config.graphics = nullptr` in `ReblueApp::OnPreSetup`, `src/reblue_app.cpp:298-300`), brings up its own plume D3D12/Vulkan renderer, and replaces the statically linked XDK D3D library function by function with `REX_HOOK` / `REX_HOOK_RAW` / `REX_STUB`. Blue Dragon is a 2006 title from the same XDK era as Conan (Conan: XDK **2.0.5632** for D3D9/D3DX9/XGRAPHC/XAPILIB/XBOXKRNL, parsed from `default.xex` optional header 0x200FF).

Our local SDK already supports this model:
- `include/rex/hook.h:36-73`: `REX_HOOK(sub, fn)` (auto-marshaled via `rex::ppc::HostToGuestFunction`), `REX_HOOK_RAW(name)` (raw `ctx`/`base`), `REX_STUB`, `REX_STUB_RETURN`. Defining a symbol with the recompiled function's name overrides it; the original body stays callable as `__imp__<name>` via `REX_EXTERN(__imp__name)`.
- `src/ui/rex_app.cpp:307-360`: if `config_.graphics` is null and `gpu_plugin` is empty, no GPU plugin loads (detached "bring your own renderer" mode). `include/rex/rex_app.h:165-185` documents the `OnCreateImmediateDrawer()` contract for ImGui overlays in detached mode.
- CAVEAT from re:Blue's CMake: hook TUs must go into an OBJECT library (or straight into the exe), never STATIC, or the linker drops the `REX_HOOK` symbols (`reblue/CMakeLists.txt`, comment above `add_library(reblue_common OBJECT)`).

rexglue-sdk `main` has no commits after v0.10.0; `development` (`923c1a5`, 9 commits) only touches UI/window/logging/kernel/xam/filesystem. **Upstream has nothing about native rendering or IGraphicsSystem replacement beyond what v0.10.0 already has.** The SDK's codegen `SigScanner` (`include/rex/codegen/sig_scanner.h`) only ships `__savegpr/__restgpr` helper signatures. There is no XDK D3D signature database anywhere upstream. re:Blue named its D3D functions by hand in IDA (`reblue/config/functions.toml:1282-1470`; `tools/shader_cache/pso_gen_templates.py:14` mentions "0x82771D18 in IDA").

### 1. UnleashedRecomp hook layer (`UnleashedRecomp/gpu/video.cpp`, 7882 lines)

Uses XenonRecomp (not ReXGlue). Hook macro: `UnleashedRecomp/kernel/function.h:351` `#define GUEST_FUNCTION_HOOK(sub, fn) PPC_FUNC(sub) { HostToGuestFunction<fn>(ctx, base); }`. Addresses are hard-coded by hand (`sub_82BDxxxx`), with no TOML list. Hook list at `video.cpp:7798-7882`:

| Guest addr | Host fn | XDK equivalent |
|---|---|---|
| 82BD99B0 | CreateDevice | Direct3D_CreateDevice |
| 82BE6230 | DestructResource | D3DResource destroy |
| 82BE9300 / 82BE7780 | LockTextureRect / UnlockTextureRect | D3DTexture_LockRect/UnlockRect |
| 82BE6B98 / 82BE6BE8 / 82BE61D0 | Lock/Unlock/GetDesc VertexBuffer | D3DVertexBuffer_* |
| 82BE6CA8 / 82BE6CF0 / 82BE6200 | Lock/Unlock/GetDesc IndexBuffer | D3DIndexBuffer_* |
| 82BE96F0 | GetSurfaceDesc | D3DSurface_GetDesc |
| 82BE04B0 / 82BE0530 | GetVertexDeclaration / HashVertexDeclaration | |
| 82BDA8C0 | Video::Present | D3DDevice_Swap/Present |
| 82BDD330 | GetBackBuffer | |
| 82BE9498 / 82BE6AD0 / 82BE6BF8 / 82BE95B8 | CreateTexture / CreateVertexBuffer / CreateIndexBuffer / CreateSurface | |
| 82BF6400 | StretchRect | engine wrapper around D3DDevice_Resolve |
| 82BDD9F0 / 82BDDD38 | SetRenderTarget / SetDepthStencilSurface | |
| 82BFE4C8 | Clear | |
| 82BDD8C0 / 82BDCFB0 | SetViewport / SetScissorRect | |
| 82BE9818 | SetTexture | |
| 82BE5900 / 82BE5CF0 / 82BE52F8 | DrawPrimitive / DrawIndexedPrimitive / DrawPrimitiveUP | D3DDevice_DrawVertices / DrawIndexedVertices / DrawVerticesUP |
| 82BE0428 / 82BE02E0 | Create/SetVertexDeclaration | |
| 82BE1A80 / 82BE0110 | CreateVertexShader / SetVertexShader | |
| 82BE1990 / 82BDFE58 | CreatePixelShader / SetPixelShader | |
| 82BDD0F8 / 82BDD218 | SetStreamSource / SetIndices | |
| 82C003B8 / 82C00910 | D3DXFillTexture / D3DXFillVolumeTexture | |
| 82E43FC8 | MakePictureData | Hedgehog engine texture load (DDS) |
| 82E9EE38 | SetResolution | |
| stubs 82BDD370 (SetGammaRamp), 82BFFF88 (D3DXFilterTexture) + ~13 unnamed | | |

NOT hooked: `SetRenderState_*`, `SetSamplerState_*`, `Set{Vertex,Pixel}ShaderConstantF/B`. These are XDK inline paths that write into the guest `D3DDevice` shadow and set dirty bits. UR reads the shadow at draw time.

**Guest device layout (UR's XDK, sizeof 0x5E00)**, `video.h:34-66`:
```
+0x000 be_u64 dirtyFlags[8]         // [0]=VS float consts (1 bit per 4 regs, MSB first), [1]=PS float consts,
                                    // [3] bit (0x8000000000000000>>(32+i)) = sampler i dirty, [4] & 0x0100000000000000 = bool consts dirty
+0x040 be_u32 setRenderStateFunctions[0x65]   // guest fn ptrs, indexed by D3DRS byte offset / 4
+0x1D4 u32    setSamplerStateFunctions[0x14]
+0x480 GuestSamplerState samplerStates[0x20]  // 6 dwords each = Xenos texture fetch constants
+0x780 u32 vertexShaderFloatConstants[0x400]  // 256 float4, big-endian
+0x1780 u32 pixelShaderFloatConstants[0x400]
+0x2780 be_u32 vertexShaderBoolConstants[4]; +0x2790 pixelShaderBoolConstants[4]
+0x2E2C be_u32 vertexDeclaration
+0x3168 viewport {x,y,w,h,minZ,maxZ} floats
```
`CreateDevice` (`video.cpp:2095-2147`) allocates this struct in the guest physical heap, points ALL `setRenderStateFunctions[]` at a host "unimplemented" thunk inserted after the code section (`g_memory.InsertFunction(PPC_CODE_BASE+PPC_CODE_SIZE+4n, HostToGuestFunction<...>)`), then installs host handlers for 17 states (`g_setRenderStateFunctions`, `video.cpp:1250-1269`: ZENABLE=40, ZFUNC=44, ZWRITEENABLE=48, CULLMODE=56, ALPHABLENDENABLE=60, SRCBLEND=72, DESTBLEND=76, BLENDOP=80, SRCBLENDALPHA=84, DESTBLENDALPHA=88, BLENDOPALPHA=92, ALPHATESTENABLE=96, ALPHAREF=100, SCISSORTESTENABLE=200, SLOPESCALEDEPTHBIAS=204, DEPTHBIAS=208, COLORWRITEENABLE=212. These are byte offsets in UR's XDK). Sampler setters are copied from the guest's own table `0x8330F3DC + i*0xC` (the XDK's static {get,set,default} table), so sampler writes keep updating the fetch-constant shadow.

Draw path: `DrawPrimitive/DrawIndexedPrimitive/DrawPrimitiveUP` (`video.cpp:4611-4700`) call `FlushRenderStateForMainThread` (`video.cpp:4297-4360`). It reads the dirty masks, snapshots only the dirty constant range into an intermediary allocator, and decodes sampler state from fetch-constant dwords 0/3/5 (`ProcSetSamplerState`, `video.cpp:4367-4413`: addressU/V/W = dword0 bits 10-12/13-15/16-18; mag/min/mip filter = dword3 bits 19-20/21-22/23-24; border = dword5 & 3). It then enqueues `RenderCommand`s into a `moodycamel::BlockingConcurrentQueue`, which a dedicated render thread consumes (`g_renderThread`, `video.cpp:5249`). All Set* hooks just enqueue commands.

Resources: guest-visible objects (`GuestTexture`, `GuestBuffer`, `GuestSurface`, `GuestShader`, `GuestVertexDeclaration`) are HOST C++ structs placement-allocated in guest physical memory (`g_userHeap.AllocPhysical<T>`). Their first bytes are a fake `GuestResource {u32 unused; be_u32 refCount; type}`, and the engine only passes the pointers back. Vertex/index buffers: `Lock` returns a guest scratch buffer, and `Unlock` byte-swaps it as 16/32-bit words into a GPU upload heap (`UnlockBuffer<T>`, `video.cpp:2225-2290`). Textures: game DDS files are loaded through an engine-level hook (`MakePictureData`, `video.cpp:5852`) -> ddspp -> host texture. UR avoids XDK tiled textures entirely.

Root signature (`video.cpp:1888-1975`): descriptor sets 0/1/2 = bindless texture heap (Texture2D/3D/Cube in spaces 0/1/2, 65536 descriptors), set 3 = bindless samplers (space3, 1024), root CBVs b0/b1/b2 space4 = VS consts / PS consts / SharedConstants, and a b3 space4 push constant for copy shaders. `SharedConstants` (`video.cpp:160-171`): `texture2DIndices[16], texture3DIndices[16], textureCubeIndices[16], samplerIndices[16], booleans, swappedTexcoords, halfPixelOffset{X,Y}, alphaThreshold`.

### 2. Shader mapping (both projects)

At runtime, `CreateVertexShader/CreatePixelShader(const be_u32* function)` receives a pointer to the XDK microcode container. Key = `XXH3_64bits(function, function[1] + function[2])` (= container `virtualSize + physicalSize`), binary-searched in the sorted `g_shaderCacheEntries[]` (UR `video.cpp:5046-5096`; re:Blue `src/gpu/shaders/guest_shaders.cpp:170-190`). Entry format (`UnleashedRecompLib/shader/shader_cache.h`, identical in re:Blue `src/gpu/shaders/shader_cache.h`):
```c++
struct ShaderCacheEntry { const uint64_t hash; const uint32_t dxilOffset, dxilSize, spirvOffset, spirvSize, specConstantsMask; GuestShader* guestShader; };
extern const uint8_t g_compressedDxilCache[]; // zstd blob of concatenated DXIL (+ size vars); SPIR-V is smol-v + zstd
```
Specialization constants (R11G11B10 normal unpack, alpha test, ...) are emulated on DXIL like this: each shader is compiled as a library with an unresolved `g_SpecConstants()`, and a generated implementation is DXC-linked at runtime (`GetOrLinkShader`, UR `video.cpp:3854`; re:Blue `src/gpu/shaders/shader_linker.cpp`, `dxc_link.cpp`). Vertex declarations: the guest `D3DVERTEXELEMENT9` (`{be_u16 stream, offset; be_u32 type; u8 method, usage, usageIndex, pad}`, `D3DDECL_END = {0xFF,0,0xFFFFFFFF,...}`) is hashed with XXH3 and converted to a native input layout (`CreateVertexDeclarationWithoutAddRef`, UR `video.cpp:4825`). Decl type codes are the Xenos-encoded values in `video.h` `GuestDeclType` (FLOAT2=0x2C23A5, FLOAT3=0x2A23B9, FLOAT4=0x1A23A6, D3DCOLOR=0x182886, SHORT2=0x2C2359, FLOAT16_2=0x2C235F, DEC3N=0x2A2187, ...). Because buffers are swapped as 32-bit words, 16-bit pairs arrive YXWZ. This is fixed in-shader with the `g_SwappedTexcoords` bitmask (re:Blue extends it to normals/tangents/binormals/blendweights/positions).

### 3. XenosRecomp (offline)

- Input: raw XDK shader containers. Directory mode (`XenosRecomp/main.cpp:75-135`) recursively reads every file and scans for `ShaderContainer` (`shader.h`): `{be flags, virtualSize, physicalSize, fieldC, constantTableOffset, definitionTableOffset, shaderOffset, field1C, field20}` with `(flags & 0xFFFFFF00) == 0x102A1100 && field1C == 0 && field20 == 0`. `flags & 1`: **0x102A1100 = pixel shader, 0x102A1101 = vertex shader** (`shader_recompiler.cpp:1111 isPixelShader = (flags & 1) == 0`). It REQUIRES the container's embedded D3DX constant table (`constant_table.h`) for register naming and cbuffer layout.
- **Conan finding:** `conan-port/game/shaders/shaders.stx` (23.7 MB) holds **1800 containers (1016 VS / 784 PS), 591 unique by SHA1**. However, **1659 of them start at file offset % 4 == 1**. Stock XenosRecomp advances `i += 4` on a non-match, so it finds only 141. Patch the scan to step 1 byte (or parse the .stx record format) before building Conan's cache. The `default.xex` image contains no embedded containers.
- Output: a `.cpp` with `g_shaderCacheEntries`, zstd DXIL (only if `XENOS_RECOMP_DXIL`) and smol-v+zstd SPIR-V. Single-file mode: `XenosRecomp <in> <out.hlsl> <shader_common.h>`. DXC via `dxc_compiler.cpp` (`-T vs_6_0/ps_6_0`, library target when spec constants are used). The re:Blue fork adds an optional 4th arg / `XENOS_RECOMP_HLSL_DUMP` env var to dump HLSL, failure collection, and `REBLUE_RECOMP` game fixes (float NORMAL/TANGENT inputs, `swapFloats` masks, PRED_SETINV fix, shadow PCF rewrite, fallback `sN_*DescriptorIndex` defines for unnamed samplers, singleton-over-array constant aliasing).
- re:Blue wires it into CMake in `cmake/shader_cache.cmake` (`reblue_shader_cache(RECOMP_TARGET XenosRecomp INPUT_DIR assets INCLUDE_FILE shader_common.h OUTPUT_CPP ...)`, globbing `*.vso *.pso *.xex`).
- `shader_common.h` conventions (upstream): VS cbuffer `b0 space4` 256 float4 (4096 B), PS cbuffer `b1 space4` 224 float4 (3584 B), shared `b2 space4`. Per-sampler `<name>_Texture{2D,3D,Cube}DescriptorIndex` sit at shared `c0..c11` (16 uints per dimension) and `_SamplerDescriptorIndex` at `c12..c15`. `g_Booleans` is at `c16.x` (VS bits 0-15, PS bits 16-31), `g_SwappedTexcoords c16.y`, `g_HalfPixelOffset c16.zw`, `g_AlphaThreshold c17.x`. Bindless heaps: `Texture2D t0 space0`, `Texture3D t0 space1`, `TextureCube t0 space2`, `SamplerState s0 space3`. Dynamic constant indices are clamped, and out-of-range reads return 0. Unimplemented: int constants, memexport, point size, dynamic GPR indexing, vfetch mini/bindings. re:Blue widens booleans to `uint4 g_BooleansArr[2]` at `c16-c17` (256 bits) and moves the rest to `c18/c19`.
- Constants must arrive little-endian. The host copies the 4 KiB guest shadow with a 32-bit byte swap per draw when dirty (re:Blue `CopyByteSwap32FlushNaN`, `constant_buffers.cpp:256-283`). That function also flushes NaNs, which is relevant to Conan's E041 NaN-constant issue.

### 4. plume RHI

`renderbag/plume` is a "lowest common denominator" RHI from RT64 for D3D12 / Vulkan / Metal. You bring your own shader compiler. About 17.3 kLOC total: `plume_render_interface*.h` ~2.4 kLOC, `plume_d3d12.cpp/.h` 4.75 kLOC, `plume_vulkan.cpp/.h` 5.1 kLOC, Metal 5 kLOC. Deps: D3D12MemoryAllocator (Windows), volk + VMA + Vulkan-Headers. Its CMake always compiles `plume_vulkan.cpp`. A D3D12-only vendor copy = compile `plume_d3d12.cpp` + `D3D12MemAlloc.cpp` + the headers and drop the Vulkan TU/deps (a small CMake edit). The Agility SDK is optional (`PLUME_D3D12_AGILITY_SDK_ENABLED`). Both UR and re:Blue use it (re:Blue pins the fork `zolaware/plume`). Alternative for us: write directly against D3D12, or reuse the SDK's `rex::ui::d3d12` provider/util (`include/rex/ui/d3d12/`), since we only target D3D12.

### 5. re:Blue implementation map (use as the primary template)

App: `src/reblue_app.cpp:289-300` (`OnPreSetup`: `config.graphics = nullptr`), `:679`/`:897` `bd::gpu::Video::CreateHostDevice(window())`, overlay via `OnCreateImmediateDrawer` -> `ImGuiOverlayDrawer`. Named function addresses live in `config/functions.toml` (`0xADDR = { name = "D3DDevice_Swap", size = 0x620 }`), which the manifest includes, so codegen emits the functions under those names and hooks bind by name.

Guest layouts are in `src/gpu/d3d.h` (Blue Dragon XDK; device alloc 0x5000, public struct 0x2A00):
- `D3DResource` 24 B `{Common(type in low 4 bits: 1 surface, 3 tex, 4 vol, 5 cube, 6 VB, 7 IB), ReferenceCount, Fence, ReadFence, Identifier, BaseFlush}`.
- `D3DTexture` 52 B (+0x18 MipFlush, +0x1C 6-dword GPUTEXTURE_FETCH_CONSTANT). `D3DSurface` 48 B. `D3DVertexBuffer/IndexBuffer` 32 B (+0x18/+0x1C fetch dwords). Shader/decl objects 24 B.
- `D3DDevice`: `m_Mask[5]` dirty u64s @0x0, `m_pRing` @0x28, `m_SetRenderStateCall[97]` @0x38, `m_SetSamplerStateCall[20]` @0x1BC, `m_GetRenderStateCall[97]` @0x20C, `m_GetSamplerStateCall[20]` @0x390, `fetchConstants[32]` @0x400, `vsFloatConstants[256][4]` @0x700, `psFloatConstants` @0x1700, VS/PS bool @0x2700/0x2710, clip planes @0x27A0, RB_BLENDCONTROL0 @0x28B8 (1-3 @0x28D8..), vertexDeclaration @0x2D10, RB_COLORCONTROL @0x2D3C (bit31 = blend enable), RT shadow[4] @0x2F88, DS shadow @0x2F98, viewport @0x3058, scissor @0x3070, PS/VS @0x3080/0x3084, IB @0x4D38.

Hooks (all `REX_HOOK` unless noted):
- `src/gpu/hooks/device.cpp`:
  - `Direct3D_CreateDevice` allocates a 0x5000-byte zeroed guest device via `SystemHeapAlloc`. `CopyDispatchTable` fills it from the XEX's static `{get,set,default}` 12-byte tables: the RS table at `0x82751D68` (0x61 entries) goes to Set @+0x38 and word0 @+0x20C; the sampler table at `0x827521F8` (0x14 entries) goes to @+0x1BC/+0x390. It also seeds RB_BLENDCONTROL=0x00010001 and a 1280x720 viewport.
  - `D3DDevice_Reset` mirrors the present params and skips PM4.
  - `Clear` has a float-arg GPR slot quirk: Z is in f1 but still consumes r8, so stencil is in r9.
  - `Swap(device, pFrontBuffer, params)` presents the resolved front buffer.
  - Also hooked: `D3DQuery_Issue/GetData`, `bdBuildGammaRampLUT`.
  - Stubs: `SetGammaRamp*`, `XGSurfaceSize`, `SetRingBufferParameters`, `RingBufferAlloc/Flush/SubmitBatch/WaitForSpace` (return 0), `RingBufferPollReady` (1), `SetShaderGPRAllocation`, `BlockUntilIdle`, `D3D__ResetAllState`, `D3D__SetSurfaceInfo`, `BeginZPass/EndZPass`, `SetScreenExtentQueryMode`, `ReleaseThreadOwnership`.
- `hooks/draw.cpp`:
  - `DrawVertices(dev, prim, startVertex, vertexCount)`.
  - `DrawIndexedVertices(dev, prim, baseVertexIndex, startIndex, indexCount)`: all 5 args must be marshaled.
  - `DrawVerticesUP(dev, prim, vertexCount, pData, stride)`.
  - `BeginVertices/EndVertices` must be hooked, or the original spins in the ring-buffer flush.
  - `Resolve(dev, Flags, pSrcRect, pDestTexture, pDestPoint, DestLevel, DestSliceOrFace, pClearColor, ClearZ..., pParameters)`.
  - `BeginTiling/EndTiling` map to clear / resolve. Quad lists are expanded with a static IB.
- `hooks/state.cpp`:
  - Hooked: `SetViewport`, `SetRenderTarget`, `SetDepthStencilSurface`, `SetScissorRect`, `SetVertexShader`, `SetPixelShader`, `SetVertexDeclaration`, `SetTexture`, `SetStreamSource`, `SetIndices`.
  - `SetRenderTarget` must maintain the device RT shadow @+0x2F88, because the unhooked `GetRenderTarget` reads it. `SetDepthStencilSurface` likewise maintains +0x2F98.
  - **SetSamplerState_* and SetRenderState_* are NOT hooked.** Their recompiled bodies keep writing the device shadow (fetch constants / register shadows).
  - The six `Set{Vertex,Pixel}ShaderConstant{FN,I,B}` are `REX_HOOK_RAW` wrappers that call the `__imp__` original and then set a dirty flag. Engine functions that write constant shadows directly (LTCG-inlined) are wrapped the same way.
- `hooks/resource.cpp`:
  - Hooked: `CreateTexture/CreateSurface/CreateVertexBuffer/CreateIndexBuffer`, `D3DVertexBuffer_Lock/Unlock`, `D3DIndexBuffer_Lock/Unlock`, `D3DSurface_LockRect`, `D3DLineTexture_LockRect`, `D3DResource_Unlock/AddRef/Release/Destroy/GetType`, `D3DTexture_GetSurfaceLevel`. `GetDesc/GetLevelDesc` get RAW wrappers.
  - Some D3D objects never go through Create*: asset-loaded VB/IB structs and hand-built `D3DTexture`s. These are bridged lazily from their fetch constants (`physical_buffers.cpp`; `native_texture_mirror.cpp` untiles DXT/RGBA8 tiled data with `GetTiledOffset2D/3D`).
- `hooks/shader.cpp`: `CreateVertexDeclaration`, `CreateVertexShader`, `CreatePixelShader`. Game-specific runtime-HLSL shader creators are replaced with precompiled host shaders.
- Host objects: `HostResourceHeap::Alloc<T>` (`src/gpu/host_resource_heap.h`) allocates the host struct inside guest memory and zeroes it. It keeps the XDK-compatible prefix at offset 0, so the engine can read Common/RefCount/fetch dwords, and registers the guest VA. `FromGuest<T>(va)` rejects unregistered pointers (engine sentinels).

Per-draw flush: `src/gpu/draw.cpp:155-331` (`FlushRenderStateLocked`). It is recorded inline on the calling guest thread under one mutex; there is no render thread. Steps:
1. Fold VS/PS/decl/strides/RT formats into `PipelineState`.
2. `ReadDeviceRenderState` (`draw.cpp:81-150`) reads blend state from `RB_BLENDCONTROL0` + `RB_COLORCONTROL` bit31 in the device register shadow. Fields: color src [4:0], op [7:5], dst [12:8], alpha src [20:16], op [23:21], dst [28:24]. Depth/stencil/cull/colorwrite come from the engine's own render-state cache global.
3. PSO lookup (by hash) only when dirty.
4. Upload VS/PS constants (4 KiB each, byte swapped) only when dirty.
5. Rebuild SharedConstants every draw: bindless texture indices, plus sampler indices decoded from `device->fetchConstants[i]` and cached per slot. The upload is skipped (memcmp) if unchanged.
6. Rebind vertex buffers/IB only for dirty ranges.

The offline PSO list is `src/gpu/pipeline/cache/pipeline_state_cache.h` (13.8k lines), plus a predictor/recorder.

Lessons to copy:
- (a) Hook the XDK D3D boundary, not PM4.
- (b) Keep the recompiled SetRenderState/SetSamplerState/constant setters and read the device shadow at draw time. This automatically covers LTCG-inlined state writes.
- (c) Maintain the shadow for every getter the engine still calls.
- (d) Stub all ring-buffer / PM4 functions so nothing waits on a GPU.
- (e) Find engine code that builds D3D objects by hand and bridge those objects.

### 6. Locating XDK D3D functions in a new title (Conan, XDK 2.0.5632)

These techniques worked here. All are reproducible from `conan-port/generated/default/*.cpp` and `conan-port/scripts/xex_decode.py`, which dumps the decrypted image at base 0x82000000.
1. **Kernel import anchors**: grep `__imp__Vd*` in the generated code, then map each hit to its enclosing `DEFINE_REX_FUNC`. `VdSwap` + `VdGetSystemCommandBuffer` -> D3DDevice_Swap. `VdInitializeRingBuffer` + `VdEnableRingBufferRPtrWriteBack` -> ring-buffer init. `VdInitializeEngines` + `VdSetGraphicsInterruptCallback` -> engine init. Following their callers leads to Direct3D_CreateDevice and the game's present routine.
2. **Static dispatch tables in .rdata**: scan the image for runs of 12-byte records `{get_fn, set_fn, default}` whose pointers fall in the D3D code range. Word1 is the setter (the larger function), word0 the getter.
3. **PM4 type-3 header immediates**: header = `0xC0000000 | (count-1)<<16 | opcode<<8 | predicate`. In the recompiled asm comments, look for `lis rX,-16384..-16381` + `ori rY,rX,<opcode<<8|p>`: DRAW_INDX 0x22 (`ori ...,8704/8705`), DRAW_INDX_2 0x36 (13824), IM_LOAD_IMMEDIATE 0x2B (11008), LOAD_ALU_CONSTANT 0x2F (12032), EVENT_WRITE 0x46 (17920/17921).
4. **Size fingerprints vs re:Blue** (`reblue/config/functions.toml`), e.g. Direct3D_CreateDevice 0xB4, D3DDevice_Swap 0x620, D3DDevice_Resolve 0xDFC.
5. Runtime confirmation: hook candidates with `REX_HOOK_RAW` pass-through wrappers that log `ctx.r3..r10` and call `__imp__`, while the Xenos path still renders.

**Preliminary Conan anchors (static evidence only; verify before relying on them):**

| Address | Proposed identity | Evidence | Confidence |
|---|---|---|---|
| `sub_822DE388` | Direct3D_CreateDevice | 45 instrs (=0xB4, same as BD); allocates **0x5700** bytes align 128 via `sub_822DE000`; returns 0x8007000E on OOM; stores device to 6th arg (r8); calls `sub_822E6C30` (ring/engine init) | high |
| `sub_822DE000` | D3D_AllocAlignedZeroed | called with (0x5700, 128) | medium |
| `sub_822E6C30` / `sub_822DFBA0` / `sub_822E6B48` | device HW init / ring buffer init / engine init | Vd* imports | high |
| `sub_822E6500` | D3D__ResetAllState-like | only reader of both dispatch tables | medium |
| `0x82A0A280` | render-state table, **91 (0x5B) entries** x 12 B `{get,set,default}` | pointers into 0x822E0xxx-0x822E1xxx | high |
| `0x82A0A6C8` | sampler-state table, 20 entries x 12 B | pointers into 0x822E1Dxx-0x822E23xx | high |
| `sub_822E8EB8` | D3DDevice_Swap(dev, pFrontBuffer, params) | only VdSwap caller; 357 instrs (BD 392) | high |
| `sub_822F5028` | D3DDevice_Resolve | 913 instrs (BD 0xDFC=895); emits DRAW_INDX/DRAW_INDX_2/IM_LOAD_IMMEDIATE/EVENT_WRITE; called right before Swap with the front buffer | high |
| `sub_82533178` | game present routine | calls `sub_822F5028` (resolve to `[0x82C81A68]`) then `sub_822E8EB8`; loads device from global | high |
| `0x82C81A64` | g_pD3DDevice (game global) | `lis -32056; +6756` used as device arg | high |
| `sub_82580918` | D3DDevice_DrawVertices (candidate) | 19 callers; builds `0xC0012201` (DRAW_INDX, 2 dwords); calls `sub_822F75F8` | medium |
| `sub_82580D00` | D3DDevice_DrawIndexedVertices (candidate) | 20 callers; builds `0xC0032201` (DRAW_INDX, 4 dwords = with index buffer) | medium |
| `sub_825803F8` | third draw variant (UP / internal IB) | 6 callers; DRAW_INDX + `0x10000002` IB-address tag | low |
| `sub_822F75F8` | dirty-state commit (SetDirtyStates-like) | called by all three draws; calls the LOAD_ALU_CONSTANT (`sub_822F7408`) and IM_LOAD_IMMEDIATE (`sub_822F72A8`) writers | medium |

**Caution:** XDK layouts differ between versions. The device size is 0x5E00 (UR), 0x5000 (BD) and 0x5700 (Conan). Conan's render-state table has 91 entries vs 97 in BD/UR. So D3DRS byte offsets and all device field offsets must be re-derived for 5632 from the setter bodies (e.g. read the Set functions at `0x82A0A280[i].set` and the `SetVertexShaderConstantF` stores). Do not copy UR/BD offsets.

## skate3recomp native renderer (researched 2026-09-24)

Sources (disposable clones, gitignored):
- `_research/upstream/skate3recomp` = https://github.com/mchughalex/skate3recomp @ `f6e0ae87fdfecbadb5c1e36c55d66a744187a3cd` (2026-07-24). Paths below are relative to its `src/`.
- `_research/upstream/rexglue-skate3` = https://github.com/mchughalex/rexglue-skate3 branch `skate3-sdk-clean` @ `7eb0faf7787f5e01333c228b8e3f03c32f7295ea` (the SDK submodule, based on rexglue 0.8.0). Paths prefixed `sdk:`.

### 0. Key conclusion: a different architecture from ours
Skate 3's "native renderer" is **not** a D3D-call / Xenos-state translator. It is a **semantic scene re-renderer**:
- Hooks sit on *engine* functions (EA RenderWare/Sk8: `RenderMesh` `sub_82795AD8`, SceneRenderView draw-list dispatcher `sub_827FAF50`, APT/HUD brackets, font emitters, movie decoder, `WorldPresentation::AddRenderInstance`), plus a few XDK D3D hooks used only for *capture* (`skate3_native_render.cpp:395-1015`).
- The guest thread records `MeshContext` pointers each frame. At the guest `D3DDevice_Swap` hook (`sub_82B82E08`, `skate3_native_render.cpp:~434`; `OnFrameEnd` at :273) it walks guest engine structures: materials by name string (`"environment.decal"`, `"tree.default"` -> `env_family`, `skate3_native_scene.cpp:~2300-2360`), world matrices, skinning palettes, and lights. It then publishes an immutable `std::shared_ptr<const FrameScene>` under a mutex (`skate3_native_scene.cpp:~10449`).
- The render callback (`RenderScene`, `skate3_native_scene_gpu.cpp:7537`) runs on the SDK command-processor thread inside the presenter's guest-output refresh (`sdk:src/graphics/d3d12/command_processor.cpp:2241-2256`, `TryRenderNativeGuestOutput`). It draws with **hand-written HLSL ports** of the game shaders (`native/shaders/scene.hlsl` uber shader + `scene_*.hlsli`, `hdr.hlsl`, `ssao.hlsl`, ...). It uses its own host RTs (HDR RGBA16F, D32, optional MSAA) and its own shadow atlas, and writes into the presenter's R10G10B10A2 guest-output texture.
- Xenos emulation keeps running (PM4, fences, queries, memexport), but **draws and resolves of suppressed passes are skipped** by surface pitch (`sdk:src/graphics/native_guest_renderer.cpp:162-181`; gates in `sdk:src/graphics/d3d12/command_processor.cpp:2547-2585` (resolves) and `2633-2667` (draws)). Passes whose output the native renderer samples from guest memory still run emulated: lightmap page composition (pitch 1024) and small composites (pitch <= 512). Depth-only draws inside those exempt passes are also skipped (`ShouldSuppressExemptDepthOnlyDraws`).
- Menus, loading, FMV, photo and CAS editor **yield** to the emulated renderer (`YieldForMenus` :5085, `YieldForMovie` :5365, ... in `skate3_native_scene_gpu.cpp`). F5 hot-toggles native/emulated. Any native failure falls back to emulation (`g_r.failed`).
- README claim: more than 2x FPS and about 1/4 of the GPU power of the emulated renderer.

Consequence for Conan: most of our open problems (EDRAM aliasing, predicated tiling, per-tile resolves, depth resolve into an atlas, rect lists) **do not exist in their design**, because they never reproduce guest RTs. What we can port directly:
- the hybrid per-pass suppression model
- the fake occlusion-query result
- the texture decode path and guarded guest reads
- XDK device shadow-bank capture hooks
- the 2D replay model
- threading/publication
- the diagnostics

### 1. EDRAM aliasing / guest RT mapping / predicated tiling
- The native path has no guest RT/depth mapping. Native RTs are fixed per purpose:
  - scene color (`g_r.hdr_scene_format` RGBA16F, or the guest output format)
  - depth D32
  - shadow atlas chain (`EnsureShadowResources`, `skate3_native_scene_gpu.cpp:3582`): 3 x `R16G16_UNORM` targets of `3*tile x tile`, raw -> hblur -> final
  - SSAO/bloom/outline targets (`EnsureOutputSizedTargets` :3993)
- Emulated passes that must still run are selected by `RB_SURFACE_INFO.surface_pitch` (default mode 2: execute only pitch == 1024 or <= 512). Suppressed passes also drop their resolves (`sdk:.../d3d12/command_processor.cpp:2549-2556`). The code comment says why: "their draws left garbage in EDRAM, and copying it out would overwrite guest texture payloads the native renderer still samples". Draw suppression and resolve suppression must agree.
- Predicated tiling: untouched. The emulated path handles it for exempt passes; native passes ignore it.
- Porting idea for Conan: our pass-runner hooks (pass fns in d3d_capture.cpp) are a better key than pitch. The same rule applies: if a pass is native, suppress both its Xenos draws and its resolves; if it is emulated, keep both.

### 2. Resolves (color/depth, atlas destination points, 24_8 reinterpretation, clear-on-resolve)
- Not implemented natively. The native renderer **re-creates** the consumers instead of resolving:
  - shadows: `RenderStaticSunMap` ~:6070, plus the dynamic cascade atlas pass (3 tiles; casters = skinned meshes + rigid props; uses the game's own captured light rows)
  - SSAO and bloom
  - DOF/postfx (`photo_fx.hlsl`, "exact ucode ports")
  - tonemap
- Their shadow atlas stores (depth, coverage) as `R16G16_UNORM`, written by a caster PSO. It is not a resolved D24S8.
- Where the game CPU-reads resolved pixels (photo grab), the SDK fork forces a synchronous resolve readback into guest memory for a length window the app arms: `native_render_force_resolve_readback_max_length` (`sdk:src/graphics/command_processor.cpp:67-104`), handled in `IssueCopy_ReadbackResolvePath` (`sdk:.../d3d12/command_processor.cpp:3367-3460`).
- Lesson for our depth->atlas problem: don't copy the 1024x1024 D24S8 into the 4096x2048 atlas with resolve semantics. Render the shadow casters straight into a host atlas region (viewport = the resolve destination point/rect) with an R32F/D32 target, and sample it with a matching host shader. This is what they do (one viewport per tile), and it removes the resolve entirely.

### 3. Rect lists / quad lists / null PS / fans / restart
- Scene meshes are re-indexed on the CPU into u16 triangle lists. Quad lists are expanded CPU-side (`DecodeMesh`, `skate3_native_scene_gpu.cpp:1188-1203`: `v, v+1, v+2, v, v+2, v+3`). Strips stay strips (`DrawEntry.prim`). The 2D replay handles prim 4/5/13 (`Draw2d`, `skate3_native_scene_state.h:573`).
- Rect lists (prim 8) and fans are never replayed: the passes that use them (postfx, resolves) are rewritten as fullscreen-triangle HLSL passes.
- Null-PS draws: shadow casters use dedicated caster PSOs (`EnsureShadowPsos` :3253). Emulated depth-only draws in exempt passes are dropped (see section 1).

### 4. Vertex/index buffers
- Each mesh is CPU-decoded into a **fixed 56-byte host layout** `{float3 pos, float2 uv, float2 uv2, unorm4 bw, u8x4 bi, float3 normal, float2 decal_uv}` (`DecodeMesh`, `skate3_native_scene_gpu.cpp:752-1215`).
  - Formats handled explicitly: pos 57/32/26; uv 25/26/31/32/37/38; k_10_11_11 normals/tangents (signed 11/11/10 unpack at :915-925).
  - u8x4 blend attributes: swap the 32-bit word, then component k = byte k (:880-893).
  - Indices: `SwapU16` (:1206-1209).
- The guest payload is first copied with an SEH-guarded memcpy (`GuestTryCopy`, `native/skate3_native_guest_read.h:36`) into a `thread_local` scratch buffer, then written once into the upload mapping. This is the same rule as our EXP-014 ("never read WC upload memory"), also stated at `skate3_native_scene_gpu.cpp:1840-1847`.
- Cache key = the guest **mesh object address**.
  - Static meshes are decoded once and prewarmed on loading screens by worker threads (`PrewarmWorkerLoop` :4663, `EnqueueMeshMiss` :4450).
  - LRU eviction: `EvictMeshStore` :429.
  - No Lock/Unlock hooks.
  - Dynamic buffers (cloth, 2D) are snapshotted on the guest thread at draw time and decoded per frame into pooled upload buffers (`AcquireMeshUploadBuffer` :715).
- 2D inline vertices come from the `D3DDevice_BeginVertices` return pointer (`sub_82B79FC0`; post-call `r3` = write pointer; `skate3_native_render.cpp:990`) and are copied at frame end.

### 5. Shader translation
- No XenosRecomp and no runtime Xenos translation. Every material family is hand-ported from disassembled ucode and "offline-validated by running the actual ucode per pixel" (`skate3_native_scene.cpp:~3400`).
- Tooling: `skate3_shader_disasm.cpp` disassembles raw big-endian `vs_*.bin`/`ps_*.bin` ucode through the SDK `Shader` analyzer (cvar `skate3_disasm_ucode_dir`).
- HLSL is compiled at runtime with `D3DCompile` plus an on-disk bytecode cache (`sdk:src/graphics/d3d12/native_rhi_d3d12.cpp:42-60, 819-835`). SPIR-V is precompiled offline and committed.
- Alpha test: a family flag in the uber shader (`clip`).
- Half-pixel offset: reproduced only for 2D ortho draws, in the VS (`native/shaders/overlay2d.hlsl:24-60`: `pos.x += 0.5*|m0.x|*w; pos.y -= 0.5*|m1.y|*w`, plus an edge snap).
- Texture swizzle: `ComposeSrvSwizzle(fetch.swizzle, host_swizzle)` into the SRV component mapping (`skate3_native_scene_gpu_internal.h:438-445`).
- Formats: only unorm/BC are mapped (`GetHostTextureFormat` :391-433: DXT1/3/5/5A/DXN, 8888, 8, 88, and 565 with a CPU R/B swap). No signed or gamma formats, and no 24_8 sampling.

### 6. Constants, per-draw cost, threading, queries
- Constants are **captured, not uploaded per draw**.
  - They hook `D3D::SetPending_AluConstants(device, u64 dirty_mask, bank, ptr)` (`sub_82B83FE0`, `skate3_native_render.cpp:821`). Bank `0x4000` = VS float bank, `0x4400` = PS bank; `ptr` = the XDK device's positional constant shadow.
  - Only the rows each ported shader needs are read, at the draw-done hook (`OnVsConstantUpload`, `skate3_native_scene.cpp:3011`). Examples: hair tint = PS c17; character lighting rows -> a canonical CB at b2.
  - Render-state bank: via `SetPending_RenderStates` (`sub_82B83C48`).
  - Vertex fetch shadow: read at `device+0x480`, 192 dwords (`skate3_native_scene.cpp:5406-5412`).
  - For Conan: check whether our XDK (2.0.5632) has the same SetPending_* split. If it does, we get dirty masks and can upload only dirty 4-register groups instead of the 8 KB per draw we copy now.
- Host binding: one root signature with 52 root DWORD constants per draw, small texture tables, and ring-buffer CBV/SRV slices for bones and shadows (`EnsureRootSignature`, `skate3_native_scene_gpu.cpp:2716-2786`; the code notes the 64-DWORD budget is full). Per-draw work = `SetRootConstants(0,52,...)` + `DrawIndexed`.
- Threading:
  - Guest capture hooks append under `g_mutex`.
  - The frame is built on the guest render thread at Swap, then handed off as an immutable shared_ptr. The render thread consumes the latest generation.
  - Texture and mesh decode run on worker threads; the render thread records their staged commits (`StagedTexCommit`, ~:540-650).
  - RHI contract: resource creation is thread-safe; command recording is render-thread-only (`sdk:include/rex/graphics/native_rhi.h:12-24`).
- Guest CPU saving: occlusion-culled static items are compacted out of the guest's sorted draw list before the guest dispatcher runs, then restored (`FilterSceneDrawList`/`RestoreSceneDrawList`, `skate3_native_render.cpp:160-220`). Guest packet building was the dominant guest cost.
- Occlusion queries: while native is active, `EVENT_WRITE_ZPD` writes a **fake positive sample count** (`query_occlusion_fake_sample_count`). Otherwise the suppressed draws would report 0 and the game would stop submitting geometry (`sdk:.../d3d12/command_processor.cpp:233-283`). This is the same fix as our EXP-013.
- Submission: while suppressing, mid-frame primary-buffer submits are skipped and everything goes into the end-of-frame submission (`OnPrimaryBufferEnd`, `sdk:.../d3d12/command_processor.cpp:2499-2510`).

### 7. Present / gamma / HDR / hacks
- The native frame is recorded inside the presenter refresh callback into the guest output. The output is R10G10B10A2 and must be returned to `kGuestOutput` state (`sdk:include/rex/graphics/native_guest_renderer.h:18-38`). The emulated gamma ramp and FXAA are bypassed for native frames.
- HDR: material branches write pre-tonemap linear color to RGBA16F. One pass then applies the game's tone chain ("fog -> exposure -> tonemap -> sqrt -> 1.41 scene multiplier") plus bloom (`native/shaders/hdr.hlsl:1-40`).
- Game-specific hacks are everywhere, and deliberate:
  - material-name families
  - two-sided sheet detection by twin triangles (`skate3_native_scene_gpu.cpp:1210-1335`)
  - front-end screen-id stacks deciding when emulated RTT passes must run (`PortraitRttWindowActive`, `skate3_native_scene.cpp:~3260`)
  - guest frame pacing (`PaceGuestFrame`, `skate3_native_render.cpp:234`)
  - ultrawide output widening

### 8. Debugging / validation tooling
- F5 native<->emulated hot toggle (A/B on the same frame), a mode indicator, and a debug dialog (`skate3_native_debug_dialog.cpp`).
- Recording: F10/F11-style multi-frame captures of all draws, with VS/PS banks, 256 render states, 192 fetch dwords, VB/IB dumps, and a guest memory snapshot for offline analysis (`native/skate3_native_diagnostics.cpp`, `skate3_native_scene.cpp:5400-5440`; cvars `skate3_native_render_snapshot_*`).
- Texture decode dumps (`skate3_native_render_scene_lm_dump` -> `native_texture_dumps/*.blk|rgba`) for byte-diffing against an offline decoder.
- Logging: MIP DIAG lines; a "suppression census" that logs each executing surface pitch once; perf lines every 600 frames (build/draw/frame avg/max).

### 9. Directly portable snippets
- Fast texture untile (`skate3_native_scene_gpu.cpp:1848-1900`):
  - Copy runs of `run_blocks = clamp(16 >> log2(bytes_per_block), 1, 8)` aligned blocks, with one `GetTiledOffset2D` call per run. Offsets inside such a run are contiguous.
  - Stage each row in a cached `thread_local` buffer and endian-swap there. Then do one memcpy into the upload heap.
  - Their comment says the old per-block loop took 3-13 ms for one 1280x720 8888 decode.
- Packed mips: fetch **mip 0 through `GetMipLocation(0, &ox, &oy, true)`** as well. Textures of 16 texels or smaller store the base level packed inside a 32x32 tile at a block offset (:1700-1708). Size tiled copies with `texture_util::GetTiledAddressUpperBound2D(right, bottom, pitch_blocks, bpb_log2)`, not `base_size`: 16bpp tiled mips reach past the linear size (:1712-1733).
- Texture identity key: FNV-1a of the 6 fetch words with the clamp bits masked (`FetchWordsKey`, `skate3_native_scene_gpu_internal.h:~172`): `key ^= k==0 ? (w & ~0x0007FC00) : w`.
- Stable fetch-word read: seqlock style, read twice until both copies match (:211-236).
- Content revalidation every 16 frames (`SamplePayloadFingerprint`, `skate3_native_scene_gpu_internal.h:217`): full FNV of payloads up to 64 KB, 64 strided qwords for larger ones.

Pass suppression gate (to adapt with our pass ids instead of pitch):
```cpp
// draw path, after shader analysis
if (!memexport_used && ShouldSuppressEmulatedDraws()) {
  if (ShouldSuppressPassAtPitch(pitch)) return true;           // native owns this pass
  if (pixel_shader == nullptr && ShouldSuppressExemptDepthOnlyDraws()) return true;
}
// copy/resolve path (edram_mode == kCopy)
if (ShouldSuppressEmulatedDraws() && ShouldSuppressPassAtPitch(pitch)) return true;  // drop resolve too
```

## The Darkness Recomp (researched 2026-09-27)

Source: https://github.com/portingpete/The-Darkness-Recomp @ `63cc32545db61c1d4dd0eeaf898f156ddd5ea413`
(2026-09-27). Re-checked the same day: UnleashedRecomp `cf829a9e` and skate3recomp `f6e0ae87`
(same commits as above) and rexglue-skate3 `7eb0faf7` for title-update support. The
generic conclusions of all three are in docs/ANY_GAME_CHECKLIST.md; this section keeps the
Darkness-specific evidence.

### 0. Key facts
- XenonRecomp (not ReXGlue) at the revision UnleashedRecomp pins, plus two patches
  (`tools/patches/`): instruction additions and a corrections patch (vcmp*h record masks
  0xFF -> 0xFFFF, fnmadd via fma, vctuxs NaN, vpkuwum/vpkuhus destination aliasing,
  mulhdu record compare, FP record forms rejected until CR1-from-FPSCR is modelled). The
  kit's ReXGlue codegen already contains the equivalent fixes (checked 2026-09-27; the pack
  aliasing case with an emulation test), except FP record forms, which it ignores silently.
- Codegen profile: every register-locality flag **off** ("correctness profile",
  `config/darkness.toml`), the opposite of UnleashedRecomp.
- Input: `_uncrypted.xex` + `basefile.exe` produced with xorloser's XexTool; README
  publishes their SHA-256 as the supported-revision check.
- Renderer: **engine-level**, D3D11. Captures completed device state and engine draw
  objects (not PM4), uses the engine's own shader sources transpiled to HLSL
  (`tools/arb_to_hlsl.py`, `batch_transpile.py`, `compile_world_*.py`). Game-specific.

### 1. Resolution / aspect (RENDERING.md "Display size and ultrawide")
- Guest video mode set to the display aspect with height <= 720 and width <= 2560, host
  integer scale 1-3x on RTs/viewports/copies. The engine then builds the Hor+ projection
  **and** its CPU visibility from the wider mode. Enlarging guest allocations directly
  overflowed a fixed rectangle descriptor and then exhausted the console texture heap:
  keep guest sizes within console limits and scale on the host (same principle as the
  kit's render_scale).

### 2. Performance findings with numbers
- Present thread at ABOVE_NORMAL: Present blocks of 1.3-1.6 s at level transitions -> worst
  5 s window < 18 ms at 120 fps.
- Unbuffered logging: 45-60 ms report blocks every 5 s -> buffered stdout, p99 < 9.2 ms.
- Descriptor-state cache with fixed-byte keys: blend lookup 312 -> 48 ns (render-thread
  samples were dominated by state hashing).
- Transient VB/IB reuse (dynamic buffers, fence-checked reserve): 96.4% reuse.
- Upload budget 4 MiB/frame with deferred draws (rejected for the kit: drops draws).
- CPU Sets worker mapping and a 30-thread helper pool: no gain / worse stutter in the
  measured heavy scene (engine thread at 96-98% of a core). ThinLTO: no gain. Lesson that
  matches Conan: the frame is bound by the single guest engine thread.
- Memory: guest C-alias region mapped as 32 views of 16 MiB over one backing store:
  82 -> 98 fps in the tunnel benchmark (specific to their runtime, not ReXGlue).

### 3. Testing
64 CTest targets: renderer "contracts" on hardware and WARP with the D3D11 debug layer
(clears in rectangles, resolves, scale 1/2/3, alpha modes, Darkness Vision stages),
kernel/dispatcher/audio contracts, synthetic submission benchmarks. Content fingerprint
of native inputs to catch stale objects after rollbacks.

### 4. Title updates in the other projects
- skate3: `cmake/ExtractTitleUpdateXexp.py` pulls `default.xexp` (and module `.xexp`) out of
  the STFS TU package; the manifest's `patched_file_path` points codegen at the staged XEX,
  with a separate `skate3_tu_functions.toml` (function boundaries differ per revision).
- UnleashedRecomp: `patch_file_path` / `patched_file_path` in the XenonRecomp config.
- Kit SDK 0.10: no manifest key, but `UserModule::LoadFromFile` applies a sibling
  `<xex>p` for codegen and runtime alike (docs/ANY_GAME_CHECKLIST.md §1).

## Survey of other ReXGlue ports (2026-09-27)

Shallow clones, HEAD commits in parentheses. Most keep the SDK's Xenos emulation; they are
read for **how ReXGlue behaves across games**, not for renderers. Generic conclusions are
merged into docs/ANY_GAME_CHECKLIST.md and PERFORMANCE_GUIDE.md (G14).

| Project (game) | SDK | Renderer | What is useful for the kit |
|---|---|---|---|
| zolaware/reblue (Blue Dragon) `ce0edad` | 0.10.0 | **native** (plume D3D12/Vulkan) | see below; the closest template to our design |
| sal063/AC6_recomp (Ace Combat 6) `09144bb0` | 0.7.8 fork `rapidsamphire/rexglue-sdk@ac6recomp-fixes` | Xenos; an experimental native "replay" renderer was **demoted** to research tooling | race-condition fixes, TU diffing, cutscene A/V resync, fps-unlock physics, full-res effects via the game's buffer registry, [rexcrt] |
| masterspike52/reNut (Banjo Nuts & Bolts) `fcdc8ba` | 0.10.0 | Xenos | XDK D3D names of a 2008 title (`config/renut_gpu_funcs.toml`), [rexcrt] incl. wide-string functions, texture dump/replace via a SetTexture hook, quality toggles as mid-asm hooks |
| masterspike52/reDAHM (Destroy All Humans! PotF) `b1a3268` | 0.10.0 (community fork `SolarRecomps/rexglue-ostentation@dev` used by the author) | Xenos | [rexcrt], split manifest (`config/*_func/_crt/_hook.toml`) |
| SolarCookies/TiP-Recomp (Viva Pinata TiP) `757f550` | 0.8.1 | Xenos | codegen middle ground: `skip_lr/skip_msr/ctr/xer/cr/reserved_as_local = true`, `non_argument/non_volatile_as_local = false`; fps/vsync/aspect hooks, shader/texture packs |
| ihatecompvir/band3_recomp (Rock Band 3, TU5) `c51944b` | 0.8.0 | Xenos | TU-based codegen, [rexcrt] |
| twist84/halo3_cache_release_recomp (Halo 3 build) `3d4a277` | ReXGlue commit `a78e0fd` | Xenos | manifest header records every hash of the input XEX |
| testdriveupgrade/TDURE (Test Drive Unlimited) `6e59fee` | 0.7.4 | Xenos | very large function list (analysis struggles on big images) |
| SkiddyToast/Crackdown (TU0) `1b89e0f`, YoshiCrystal9/re-gh2 `ef521f4`, PranchaD/re-Sonic06Demo `4b78cc0` | 0.2-0.x | Xenos | minimal configs: setjmp/longjmp addresses are the one thing every project sets |
| rjkiv/dc3-decomp (Dance Central 3, **decompilation**, CC0) `149a613` | - | - | full symbol map incl. XDK libraries -> `tools/re/xdk_2012_dc3_symbols.tsv` (6,200 D3D/CRT/xapilib/xgraphics/d3dx9 functions with sizes and object files) + `tools/re/xdk_layout.py` |

### Cross-project facts
- **setjmp/longjmp**: set by 11 of 12 manifests. ReXGlue has no auto-detection; the kit
  now finds them with xdk_layout.py (late XDK) or by signature/semantics.
- **[rexcrt]**: 6 projects map guest CRT/heap/file/string functions to the SDK's native
  implementations (`sdk/src/kernel/crt/`: ~70 functions incl. fibers; heap needs all four
  Rtl*Heap and `rexcrt_heap_enable`). Conan used none -> PERFORMANCE_GUIDE G14.
- **Split manifests** (`includes = [...]` with func/crt/hook files) keep hundreds of
  overrides reviewable (reblue: one TOML per subsystem under `config/hooks/`).
- **SDK forks are common** (AC6, reDAHM, skate3, ours). Record fork changes like
  `sdk/KIT_SDK_CHANGES.md` does, and check community forks for fixes before debugging an
  SDK-level problem.

### AC6: races that the 360 never showed (`src/ac6_backend_fixes/`)
- `ac6_effect_mode_fix.cpp`: one-frame effect/cloud flicker. Update and draw jobs share a
  mode word written by the submitter and read later by a pool worker. On the 360, fixed
  core assignment and guest priorities kept them ordered; "rexglue discards both guest
  priorities and guest affinities". Kit SDK defaults confirm it:
  `ignore_thread_priorities=true`, `ignore_thread_affinities=true`
  (`sdk/src/system/xthread.cpp:40-43`).
- `ac6_storage_submit_order_fix.cpp`: job op-code stored after Submit -> wrong save
  operation. Found by **diffing the retail title update against the base XEX**: the
  developer had already fixed it. Technique: when porting a base revision, diff the TU for
  shipped fixes of timing bugs.
- `ac6_cutscene_resync.cpp`: in-engine cutscenes tick per rendered frame while audio runs
  on its own clock; a long frame desyncs them permanently. Audio-master catch-up (bounded
  extra ticks when >= 2 behind).
- `ac6_fps_physics_fix.cpp`: fps unlock broke flight dynamics that accumulate fixed
  per-frame steps; scaled per call by the real delta.
- `ac6_fullres_effects.cpp`: half-res effect chain moved to full res by rewriting the
  game's own render-buffer registry at init (same EDRAM tile footprint), so every
  downstream size stays self-consistent.
- Renderer pivot: the native replay renderer competed with Xenos as a second "render
  authority" and was demoted to tooling. Lesson consistent with the kit: one authoritative
  output path, the other only as an A/B reference.

### reblue: native-renderer pieces worth copying (`src/gpu/`, `config/hooks/`)
- `pso_predictor.cpp` + `pso_predictor.toml`: brackets the engine's model build inside the
  async load callback, predicts every (technique, vertex decl) PSO the model will need and
  **gates the load completion until they are compiled** (watchdog-bounded). Concrete
  template for G13.
- `occlusion.cpp`: native occlusion queries as UAV counters read back kNumFrames later
  (for the one query family the game uses, the sun). Template for G3.
- `physical_buffers.cpp`: VB/IB registered at the engine's `XGOffsetResourceAddress`
  calls during scene-graph build, freed at `XPhysicalFree`: persistent host mirrors
  instead of per-draw dirty tracking. Alternative to the kit's page-dirty tracking for
  static geometry when upload/planning cost shows up (G6/G7 family).
- `native_texture_mirror.cpp`: host textures created at guest texture creation, evicted on
  the engine's free path.
- `dred.cpp`: DRED dump windowed to the unfinished ops (NATIVE_RENDERER_ARCHITECTURE §8).
- `engine/frame_interp.cpp` (3.3k lines): fps unlock by **render interpolation** between
  30 Hz logic ticks (cameras, bone palettes via polar decomposition), the alternative to
  delta-time fixes when the game logic cannot run faster.
- `output_resolution.toml`: native output size by patching the device backbuffer dims,
  the per-frame force-to-1280 and the projection aspect at each place it is built.

## LostOdysseyRecomp (researched 2026-09-27)

Source: https://github.com/freefrank/LostOdysseyRecomp @ `81fe304` (2026-09-27), v0.7.3.
XenonRecomp + XenosRecomp + plume (UnleashedRecomp lineage, not ReXGlue); Unreal Engine 3
game. Renderer: Xenos PM4/register interpretation with ucode -> HLSL -> DXIL on plume
D3D12/Vulkan (their roadmap removes the "legacy PM4 packet translation layer"), so the
architecture is closer to a lean Xenia than to the kit's XDK hooks. Its value for the kit is
the **measurement discipline and the concrete findings** (docs/notes/, mostly Chinese).

### Findings, with their numbers
- **Depth-clear coalescing** (`vulkan-depth-clear-performance-2026-09-13.md`): 720 EDRAM
  tile rectangles per clear coalesced exactly into 1: 4K Vulkan 7.5 -> 43.5 fps, GPU
  132.9 -> 21.1 ms, record 37.3 -> 4.9 ms. The kit hooks D3DDevice_Clear with the game's own
  rects (0-1 usually), so the pathological case is unlikely, but the rule is generic: union
  clear/resolve rects before issuing, and a rect set covering the whole surface becomes a
  full clear (NumRects = 0) so the driver can fast-clear (NATIVE_RENDERER_ARCHITECTURE §8).
- **Submission ring** (`reblue-gpu-comparison.md`, `perf-gpu-ring-compare.md`): the old
  renderer waited on the fence right after each mid-frame Flush (~12-15 ms of a 21 ms draw
  phase). Two slots, submit without waiting, wait only when a slot is reused: fence_wait
  0.0007 ms mean. Same contract as the kit's frames-in-flight + upload overflow pages.
- **Root signature / descriptor-table de-duplication** in plume (Issue #70), counters only
  behind `LO_RENDER_TIMING`.
- **CPU guide** (`cpu-performance-optimization-guide.md`): measure first (fence waits, mid-frame
  splits, memcmp share per thread, hash chains), then single-thread waste, only then bounded
  "parallel prepare / serial commit". Rejected with evidence: hand-written register SIMD for
  constants (1.53 -> 2.16 ms), default thread pinning, a 6-worker Xenon-mimicking scheduler,
  parallelizing the game's render function. A vertex-compare SIMD gave 9-15x in isolation
  but +0.6% fps because the GPU dominated at 4K: always check the whole frame.
- **Wait-path modernization**: kernel Event/Semaphore/Mutant timed waits polled every
  200 us; replaced by condition-variable waits with deadlines (their runtime, not ReXGlue;
  the kit's equivalents are PERFORMANCE_GUIDE item 7).
- **"3C6T" handheld envelope** (`cpu-card-d-3c6t-city-2026-09-14.md`): run the whole process
  with affinity 0x3F (3 physical cores / 6 logical CPUs) as a handheld proxy; results must
  hold there, not only on a 16-thread desktop. Found: shader preparation sized its pool from
  `hardware_concurrency()` (16 -> 15 workers) instead of the process affinity.
- **Shader discovery at UE3 scale** (`shader-preparation.md`): 20,686 source shaders inside
  compressed UE3 packages (CPX/FPD); a built-in layout index cut discovery 74 s -> 13 s and
  application reads 15.2 GB -> 142 MB; 28,484 shaders prepared at startup in parallel with a
  skippable progress screen and a persistent cache.
- **Recompiler semantic audit** (`recompiler-width-audit.md`): 9 defect classes in XenonRecomp
  validated by 3,258 generated-instruction checks (update-form loads/stores high word,
  RLWIMI wrapping masks, SRAW/SRAD carry, 18 missing Rc forms, atomic/absolute address
  wrap, BLRL trap, bctr low bits, BDNZF bit selection). Checked against the kit SDK on
  2026-09-27: BLRL, RLWIMI and BDNZF are already correct; **SRAW/SRAD carry was wrong and is
  fixed** (sdk/KIT_SDK_CHANGES.md, scripts/tests/test_codegen_sra.py); bctr low bits are
  harmless for valid code; the Rc-form and high-word items remain an open audit for the kit.
- Codegen generation guard: hashes of generator, config and outputs checked before build
  (stale generated code after a generator change was a real failure there).

## VivaPinataRecomp (researched 2026-10-01)

https://github.com/crabinacrabic/VivaPinataRecomp @ efb21f6 (no LICENSE: ideas only, no code
copied). ReXGlue 0.10.0.8-dev nightly (`nightly-20260915-1406e1b7`, fetched as a release zip),
Xenos D3D12 renderer (ROV/RTV option), Visual Studio 2026 + clang 22, launcher EN/RU.
- **Codegen bug, fixed in the kit SDK**: `vpkd3d128` FLOAT16_4 with vD == vB lost the sign of
  lane x (the game's float->half idiom `lvlx v0 / vpkd3d128 v0,v0,5,2,2 / vsplth / stvehx`; the
  terrain grid folded into one quadrant). They patched 30 sites with midasm hooks; the kit fixes
  the builder for every in-place pack type (sdk/KIT_SDK_CHANGES.md) and the test-harness bug
  that hid half of the PPC tests.
- **Adjustor thunks** (`tools/find_thunk_holes.py`): MSVC `addi r3,r3,-N; b` thunks reached only
  from undiscovered vtables -> kit `scripts/port/find_adjustor_thunks.py`.
- **Runtime sweeps** (`src/debug_tools.h`, cvar `dev_debug_runtime`): a data-pointer scan
  (runs of code pointers in data sections = vtables; unknown targets -> new functions) and a
  stub sweep, classified by `tools/data_pointers_to_toml.py` / `stub_sweep_to_toml.py` into
  NEW FUNCTION / ALREADY KNOWN / MID-FUNCTION (a missed jump table, not a function). Worth
  porting when a game has many vtable crashes.
- **Manifest pre-flight** (`tools/validate_manifest.py`): checks includes, known keys,
  alignment/range of addresses, size/end exclusivity, [rexcrt] names (heap group
  all-or-nothing), setjmp/longjmp together, before a long codegen.
- Manifest split by subsystem (`config/*_functions/_hooks/_midasm/_crt/_ctx.toml`), overrides
  as strong `extern "C"` symbols replacing the weak generated ones (`src/game_fixes.h` header
  lists the recipes: wrapper, replacement, typed hook, stub, call-in, midasm).
- Timing: `timeBeginPeriod(1)` + hybrid sleep (TiP-Recomp idea), already in the kit SDK (E049).
- Language: SDK `user_country` 103 -> the game reads `englishus.bnl` (ANY_GAME_CHECKLIST §4).
- Gotchas: non-ASCII project path crashes codegen (0xC0000409); first-chance AVs from the
  SDK write-watch stop debuggers (LESSONS A/C); in-place inflated archives must be rebuilt
  with exactly the original compressed size (their `make_russian_bnl.py`), a rule for any
  data mod.
