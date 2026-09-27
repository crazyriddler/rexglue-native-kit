# Renderer Analysis — Conan (2007, Nihilistic) on ReXGlue v0.10.0

Status: M2 static recovery, first pass (2026-09-24). All addresses are guest VAs in
`default.xex` (image base 0x82000000). The generated C++ symbol for guest function
`0xXXXXXXXX` is `sub_XXXXXXXX`; the original body is always reachable as
`__imp__sub_XXXXXXXX` (weak-alias scheme in `generated/default/conan_pch.h`), so a
strong `REX_HOOK(sub_XXXXXXXX, fn)` / `REX_HOOK_RAW(sub_XXXXXXXX)` in `conan-port/src`
replaces every static *and* indirect call (indirect calls resolve through
`conan_register.cpp`, which registers the same weak symbol).

Evidence was produced with the scripts in `tools/re/` (see "Tooling" at the end).
Confidence: H = confirmed by several independent facts (table decode, packet
constants, argument usage, callers), M = consistent behaviour + one strong clue,
L = plausible label only.

## 1. Big picture

Conan uses a **statically linked Xbox 360 XDK Direct3D 9 (circa 2006/2007)** plus
the **D3DX Effect runtime**. The engine never writes PM4 itself except through the
XDK. The D3D library lives in two blocks:

* `0x822DD000–0x822F9D00` — D3D core (device, ring buffer, state setters, resources,
  Resolve, tiling, Swap, PIX capture).  `0x822F9D00–0x822FB000` is XGraphics/D3DX math.
* `0x8257C000–0x82581160` — second LTCG-placed D3D block holding the **draw entry
  points** (DrawVertices / DrawIndexedVertices / Begin/EndVertices / DrawVerticesUP)
  and misc helpers. `0x82581160+` is D3DX texture loading.

The game renderer layer is `0x824D0000–0x82555000` (render-pass table, batch
renderer, material/shader binding, post effects), and the D3DX effect/state-manager
lives around `0x825B0000–0x825B8000`.

Almost all state is **shadowed in the guest `D3DDevice` struct** and only turned into
PM4 inside the draw prologue ("SetPendingState") — exactly the reblue / Unleashed
situation, so API-level hooks can read complete draw state from the device struct.

### Call graph (recovered)

```text
render thread frame  sub_824D3870 (indirect; no static callers)
  ├─ D3DDevice_AcquireThreadOwnership 822DE180
  ├─ ... game scene render (virtual) sub_822464C8
  │     ├─ sub_824DECD0 (frame begin)            
  │     ├─ sub_824DE7A0 -> sub_824F1E70  RENDER-PASS RUNNER (29 passes, table 0x82A25A70)
  │     │     for each enabled pass: GPU timer marker sub_8253E510 (SetPredication +
  │     │     D3D callback 822DFB48), perf marker sub_824DEC98(name), then pass fn(ctx)
  │     │       "Render Shadow Maps"   824F1880
  │     │       "Begin Tiling"         824F1A40 -> 8254DB80.. -> D3DDevice_BeginTiling 822F3EF8
  │     │       "Render Depth Only"/"Decal"/"Opaque"/"Opaque Character"/"Ocean"/"Sorted"
  │     │           -> 824FB6xx -> sub_824F1110 BATCH RENDERER
  │     │                 per batch (20-byte entries, 64 per group, optional occlusion
  │     │                 predication 822EAB08/822EACB0):
  │     │                   sub_824F0B98 (prepare 396-byte draw record)
  │     │                   sub_8254D030 -> jump table 0x82A27AB4[type]
  │     │                        type0 8254CBE8 -> renderable->vtbl[+0x1C] (object Draw)
  │     │                        type1 8254CEF0 -> callback vtbl[+0x24]
  │     │                        type2 8254CC18 -> 82548FA8 (stencil/HiStencil special)
  │     │                 object Draw bodies (e.g. 82541100, 82542100, 825429F0..82544118,
  │     │                 82536830, 825370D8, 82538ED0, 825478E8, 82547C78):
  │     │                   material bind  sub_82534758 (SetPixelShader+SetVertexShader)
  │     │                   commit         sub_82534F10 -> D3DX state mgr 825B6D58
  │     │                                  (SetTexture, VS/PS float consts written
  │     │                                   directly into dev+0x780/0x1780, B/I consts)
  │     │                   SetStreamSource/SetIndices/SetVertexDeclaration
  │     │                   D3DDevice_DrawIndexedVertices 82580D00 / DrawVertices 82580918
  │     │       "Deferred Shade"       824F21D8
  │     │       "End Tiling"           824F1CC0 -> D3DDevice_EndTiling 822F4480 (-> Resolve)
  │     │       "Resolve HDR Texture"  824F1D80 -> D3DDevice_Resolve 822F5028
  │     │       "Render Post Process"  824F1DE0 -> post-effect objects (Bloom 82551030,
  │     │           ToneMap/Bloom composite 82553CB0, DoF 825547A8, HSV 82552830,
  │     │           Outline 82552CB8, AutoExposure 82553890)
  │     │       "Upscale" 824F1E38, "Render Movie" 824F1E40, "Render HUD" 824F1E68
  │     └─ sub_824DED00 (frame end)
  ├─ sub_82533178  PRESENT: SetRenderState_PRESENTINTERVAL,
  │     D3DDevice_SynchronizeToPresentationInterval? 822E8EB0,
  │     D3DDevice_Resolve(dev,0,NULL,frontBuffer=*(0x82C81A68),...),
  │     D3DDevice_Swap(dev, frontBuffer, NULL) 822E8EB8 -> VdSwap @0x822E9104
  └─ D3DDevice_ReleaseThreadOwnership 822DE1C0

D3D draw prologue (inlined into 82580918 / 82580D00 / 825803F8):
  m_Mask[0] -> 822F6860(VS consts dev+0x780 -> reg 0x4000)
  m_Mask[1] -> 822F6860(PS consts dev+0x1780 -> reg 0x4400)
  m_Mask[2] -> 822F75F8 (shaders/decl/SQ), 822F6128 (tiled/predicated state), 822F64C0 (reg ranges)
  then PM4: type0 VGT_INDX_OFFSET(0x2102), DRAW_INDX 0xC0012201 / 0xC0032201
ring: dev+0x30 write ptr (pre-increment), make-space 822DF848 -> KickOff 822DF100
  -> 822DEC70 writes INDIRECT_BUFFER (0xC0013F00) into primary ring and CP_RB_WPTR (MMIO 0x7FC80714)
```

## 2. Global anchors

| Address | Meaning | Evidence | Conf |
|---|---|---|---:|
| `0x82C81A64` | game's `IDirect3DDevice9*` (`g_pd3dDevice`) | stored by `Direct3D_CreateDevice(…, ppDevice=0x82C81A64)` in 825329A8; 787 `lwz` refs in 183 game funcs | H |
| `0x82C81A68` | front-buffer texture (Resolve dest / Swap arg) | `CreateTexture` result stored at 825329A8; passed to Resolve+Swap in 82533178 | H |
| `0x82000584` | import of kernel variable `VdGlobalDevice` (xboxkrnl ord 0x1BE) | image word = 0x000101BE; 20 loads in 19 D3D funcs (e.g. D3DResource fence helpers 822DF430) | H |
| `0x82000660` | import of `KeDebugMonitorData` (ord 0x59) | image word 0x00010059; used by device destroy / PIX | H |
| `0x82A0A208` | render-state dispatch template: 101 × {Get,Set,Default} (first 10 rows are no-op stubs 822CC828/82476728, so `D3DRS_ZENABLE == 40`) | copied into device by `D3D__ResetAllState` 822E6500 (loop 404/4 entries; `dev+0x40+4i=Set`, `dev+0x224+4i=Get`, then calls Set(Default)) | H |
| `0x82A0A6C8` | sampler-state template: 20 × {Get,Set,Default} | same function, second loop (`dev+0x1D4`/`dev+0x3B8`), 26 samplers | H |
| `0x82A25A70` | **game render-pass table** 29 × {enabled, name*, fn*, category} | names "Render Shadow Maps", "Begin Tiling", … ; iterated by 824F1E70 | H |
| `0x82A27AB4` | batch-type draw jump table (3 entries) | used by 8254D030 | H |
| `0x8201A868` | vtable of the PIX GPU-capture recorder installed at `dev+0x5404` | ctor 822F89E0/822F8A40, created only by PIX state machine 822ED860 ("PIX!Gpu", "unnamed.pix2") | H |

Note on earlier notes (`conan-port/docs/error_log.md`): the "interface at object+21508"
seen in `sub_822DEC70` is `dev+0x5404`, the **PIX capture recorder**, and
`sub_822F8300` is that recorder's PM4 walker (opcodes 0x37/0x3F = INDIRECT_BUFFER),
not a game shader-parameter interpreter. `sub_822DEC70` is D3D KickOff (primary-ring
INDIRECT_BUFFER writer), not a per-object draw. Both are irrelevant to game rendering
unless a PIX capture is active.

## 3. Recovered D3D API (table)

Callers = static `bl`/tail-`b` sites (from `tools/re/d3d_symbols.tsv`). Hook value:
5 = primary native-renderer boundary, 1 = informational.

| Guest addr | Proposed name | Role / signature | Callers | Evidence | Conf | Hook |
|---|---|---|---:|---|---:|---:|
| 822DE388 | Direct3D_CreateDevice | (adapter, type, hwnd, flags, pp, ppDevice); allocs 0x5700-byte device | 1 | "0x8007000E" error, alloc 22272 bytes/128-aligned, calls 822E6C30, caller stores to 0x82C81A64 | H | 5 |
| 822E6C30 | D3DDevice init body | ring setup, ResetAllState, dummy DRAW_INDX_2 buffers (822E7900) | 1 | calls 822DFBA0/822E6500 | H | 2 |
| 822DE128 / 822E6EB0 | D3DDevice_Release / destroy | refcount at dev+0x3C | 4 | VdShutdownEngines, VdInitializeRingBuffer(0) | H | 2 |
| 822DFBA0 | D3DDevice_SetRingBufferParameters | VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, ME_INIT 0xC0114800 | 3 | game passes 0x40000 primary / 0xA00000 secondary | H | 3 |
| 822DF848 | D3D ring make-space | called when `dev+0x30 > dev+0x38`; returns new write ptr | 79 | | H | 3 |
| 822DF100 | D3D KickOff | fence bookkeeping, then 822DEC70 | 3 | | H | 3 |
| 822DEC70 | AddCallsToPrimaryBuffer | INDIRECT_BUFFER 0xC0013F00 into primary ring (dev+0x3A18), `stw CP_RB_WPTR` @ 0x7FC80714 | 4 | | H | 3 |
| 822DFA68 | D3D RingAlloc(dwords) | | 13 | | H | 2 |
| 822DFAD0 | D3DDevice_BlockUntilIdle | type0 reg 0x5C8, spins on dev+0x2AF8 | 11 | | M | 2 |
| 822DF1D8 / 822DF430 | BlockOnFence / D3DResource_BlockUntilNotBusy | | 10/1 | | M | 2 |
| 822DE180 / 822DE1C0 | Acquire/ReleaseThreadOwnership | dev+0x2A88 = owner thread | 4/4 | | M | 1 |
| 822DFB48 | D3DDevice_InsertCallback | used by game GPU timer 8253E510 | 2 | | M | 1 |
| 822E8EB8 | **D3DDevice_Swap** | (dev, pFrontBuffer, pParams); VdGetSystemCommandBuffer + **VdSwap** @0x822E9104 | 2 | only VdSwap caller | H | 5 |
| 822E8EB0 | Swap-sync helper (→822E8A10, r4=0) | probably SynchronizeToPresentationInterval | 1 | WAIT_REG_MEM in callee | M | 2 |
| 822E2F48 (thunk 822E3BE8) | **D3DDevice_SetRenderTarget** | (dev, index, surface) → dev+0x3090+4i | 12 | `addi r11,r4,3108` | H | 5 |
| 822E32B0 | **D3DDevice_SetDepthStencilSurface** | (dev, surface) → dev+0x30A0 | 8 | | H | 5 |
| 822E3588 | D3DDevice_SetSurfaces | (dev, D3DSURFACES*) tiling variant: RT0..3+DS | 4 | stores 12432..12448 & 12716..12736 | M | 4 |
| 822E2878 | D3DDevice_GetRenderTarget | AddRef + return | 1 | | H | 1 |
| 822E2E10 / 822E2B98 | **D3DDevice_SetViewport** / SetViewportF | (dev, D3DVIEWPORT9*) → floats at dev+0x3160..0x3174; PA_CL_VPORT_* shadow dev+0x2908..0x291C | 5 / 2 | int→float conversion, 6 fields | H | 5 |
| 822E25C8 | **D3DDevice_SetScissorRect** | (dev, RECT*) → dev+0x317C; PA_SC_WINDOW_SCISSOR shadow dev+0x28C4/0x28C8 | 5 | | H | 4 |
| 822E2A50 | D3DDevice_SetClipPlane | (dev, i, float4*) → dev+0x2820+16i | 1 | | H | 3 |
| 822E26C8 | **D3DDevice_SetStreamSource** | (dev, stream, vb, offset, stride, fetchDirtyBit) → vertex fetch const dev+0x778−8s, vb ptr dev+0x30A4+4s, stride/4 byte dev+0x30E8+s | 47 | fetch = (vb+0x18)+offset, size = vb+0x1C − offset | H | 5 |
| 822E27E8 | **D3DDevice_SetIndices** | (dev, ib) → dev+0x308C | 25 | DrawIndexed reads dev+12428 | H | 5 |
| 822E8200 | **D3DDevice_SetVertexDeclaration** | (dev, decl) → dev+0x2E24, dirty m_Mask[2] bit 0x80000 | 47 | read by 822F75F8 | H | 5 |
| 822E86C0 | D3DDevice_SetFVF | → dev+0x2E24 + dev+0x3088 | 10 | | M | 3 |
| 822E7CE8 | **D3DDevice_SetVertexShader** | (dev, vs) → dev+0x318C, deferred release of old | 3 | | H | 5 |
| 822E7FE8 | **D3DDevice_SetPixelShader** | (dev, ps) → dev+0x3190 | 3 | | H | 5 |
| 822E7EA8 / 822E81B8 | GetVertexShader / GetPixelShader | | 1/1 | | H | 1 |
| 822E5BE0 | **D3DDevice_SetTexture** | (dev, sampler, tex, fetchDirtyBit) → tex ptr dev+0x30F8+4s, fetch const dev+0x480+24s built from tex+0x1C..0x30 | 9 | | H | 5 |
| (dev+0x1D4 table) | SetSamplerState_* (20 fns 822E1BD8..822E2568) | write fetch-const bits at dev+0x480+24s; **no static callers** — game sets sampler state only via D3DX effect / dispatch table | 0 | table 0x82A0A6C8 | H | 4 |
| 822E0290… (91 fns) | SetRenderState_* (full list: `tools/re/statetables.py`) | e.g. CULLMODE 822E0290 (39), HALFPIXELOFFSET 822E18B0 (47), ALPHABLENDENABLE 822E0328 (23), SRCBLEND 822E0448, DESTBLEND 822E04D8, BLENDOP 822E03B8, ALPHATESTENABLE 822E02F0, ALPHAREF 822E0750, ZENABLE 822E09E0, ZFUNC 822E0A50, ZWRITEENABLE 822E0A20, STENCIL* 822E0A88.., COLORWRITEENABLE 822E0F78, DEPTHBIAS 822E0E80, SLOPESCALEDEPTHBIAS 822E0DB8, VIEWPORTENABLE 822E14F8, HISTENCIL* 822E1AF8.., SCISSORTESTENABLE 822E2E00 | ~500 total | template table 0x82A0A208 + defaults match XDK (CULL_CCW=6, CMP_LESSEQUAL=3, BLENDFACTOR=~0, ALPHATOMASKOFFSETS=0x87, HIZ=AUTOMATIC=2 …) | H | 4 |
| 822E7B78 / 822E7BD8 | SetVertex/PixelShaderConstantB | dev+0x2780 / 0x2790 | 2/2 | | H | 4 |
| 822E7C38 / 822E7C90 | SetVertex/PixelShaderConstantI | dev+0x27A0 / 0x27E0 | 2/2 | | H | 4 |
| (inline) | SetVertex/PixelShaderConstantF | **inlined** into D3DX state manager 825B6D58 / 825B2F28 (+ 8235E7E0, 82345AC8): stores to dev+0x780+16r / dev+0x1780+16r and ORs m_Mask[0]/[1] | — | `(reg+120)*16`, `(reg+376)*16` address math (`tools/re/constwriters.py`) | H | 5 |
| 822E7A48 | D3DDevice_GpuBeginShaderConstantF4 | direct ring allocation of constants (type0 0x4000/0x14000) and clears dirty bits | 1 (824DB7B8) | | M | 4 |
| 82580358 | Gpu SET_CONSTANT writer (0xC000nn2D00) | used by 824DB6B8/824DC540/824DC8C8 | 3 | | M | 3 |
| 822E8390 | D3DDevice_SetShaderGPRAllocation | (dev, flags, vsGPRs, psGPRs), default 64/64 | 16 | dev+0x2AA8 | M | 1 |
| **82580918** | **D3DDevice_DrawVertices** | (dev, PrimType, StartVertex, VertexCount) | 35 | inlined SetPendingState; type0 `0x2102` VGT_INDX_OFFSET, `DRAW_INDX 0xC0012201`, initiator = count<<16 \| 0x80 \| prim; splits >65535 | H | 5 |
| **82580D00** | **D3DDevice_DrawIndexedVertices** | (dev, PrimType, BaseVertexIndex, StartIndex, IndexCount) | 28 | `DRAW_INDX 0xC0032201`, index base from `*(dev+0x308C)+0x18`, INDEX32 = ib Common bit31 | H | 5 |
| 825803F8 | D3DDevice_BeginVertices | (dev, prim, vertexCount, stride) → ring ptr | 7 | | H | 5 |
| 82580898 | D3DDevice_EndVertices | restores ring from dev+0x3474 | 6 | | H | 5 |
| 825808B8 | D3DDevice_DrawVerticesUP | Begin + memcpy + End | 13 | | H | 5 |
| **822F5028** | **D3DDevice_Resolve** | (dev, Flags, pSrcRect, pDestTex, pDestPoint, Level, Slice, pClearColor, f1 ClearZ, ClearStencil, pParams) | 44 | EVENT_WRITE/INVALIDATE_STATE/WAIT_REG_MEM, RT index = Flags&7 (4=depth), clear flags 0x100 (RT) / 0x200 (DS) | H | 5 |
| 822F3EF8 | D3DDevice_BeginTiling | predicated tiling setup, SET_BIN_MASK, viewport | 1 | size & callees match XDK | M | 5 |
| 822F4480 | D3DDevice_EndTiling | per-tile Resolve ×2, SetPredication | 1 | | M | 5 |
| 822F3D88 | D3DDevice_SetPredication | (dev, tileMask) → dev+0x31A4, BIN_MASK_LO | 13 | | M | 4 |
| 822EAB08 / 822EACB0 | begin/end conditional (per-batch viz-query predicated) rendering | used by 82548778 etc when batch has query id | 11/11 | BIN_MASK pair, id arg | L | 3 |
| 822EA978 / 822EAA48 | Begin/End visibility (occlusion) query | VIZ_QUERY 0xC0002300 | 3/3 | | M | 3 |
| 822DE1D0 | FlushHiZStencil (probable) | type0 0x5C8 / EVENT_WRITE 15 | 4 | | L | 2 |
| **822E5878** | **D3DDevice_CreateTexture** (internal form) | probable (W, H, Depth, Levels, Usage, Format, Pool, Type=3) → returns 52-byte D3DBaseTexture + phys alloc (arg order M) | 23 | calls XGSetTextureHeaderEx 822E50B8; init call passes (w,h,1,1,1,0x28280106,0,3) | H | 5 |
| 822E5998 / 822E5630 / 822E5700 / 822E57C8 | other texture/surface creators (cube/volume/surface) | | 6/7/4/4 | call 822E49F8/822E53C8 | L | 4 |
| 822E9FC0 | **D3DDevice_CreateVertexBuffer** | (Length, Usage, …) → 32-byte header, fetch dword0 = addr\|3 | 19 | | H | 5 |
| 822EA0E8 | D3DDevice_CreateIndexBuffer | | 9 | twin of above | M | 5 |
| 822EA088 / 822EA0D8 | D3DVertexBuffer_Lock / Unlock | | 20/20 | | H | 5 |
| 822EA198 / 822EA1E0 | D3DIndexBuffer_Lock / Unlock | | 10/9 | | H | 5 |
| 822E8308 (→822E8218) | D3DDevice_CreateVertexDeclaration | (D3DVERTEXELEMENT9*) → 56+12n decl object | 1 (8254FB38) | stops at Stream==0xFF | H | 5 |
| 822E85D0 / 822E84C0 | CreateVertexShader / CreatePixelShader | microcode copy (8257C8A8), called by D3DX effect loader 825B2E68 | 1/1 | | M | 5 |
| 822E9EF0 | D3DResource_Release | atomic refcount at +4 | 68 | | H | 4 |
| 822E98C0 | D3DResource_GetType | | 11 | returns 16..20 like BD | H | 1 |

## 4. Recovered data structures

### D3DDevice (0x5700 bytes, this XDK revision)

| Offset | Field | Evidence |
|---|---|---|
| 0x000 | `u64 m_Mask[6]` dirty masks: [0]=VS float consts, [1]=PS float consts, [2]=render-state/shader/decl groups, [3]=fetch consts (textures/streams), [4]=misc (bool/int consts, clip planes, half-pixel, scissor) | draw prologue tests each qword; setters OR bits |
| 0x030 | `m_pRing` (last written dword; packets written with `stwu x,4(p)`) | Swap/Resolve/draws |
| 0x034 | `m_pRingLimit` | RingAlloc compares ptr+n |
| 0x038 | `m_pRingGuarantee` (make-space threshold) | `if (p > guarantee) MakeSpace` |
| 0x03C | reference count | Release |
| 0x040 | `m_SetRenderStateCall[101]` | ResetAllState |
| 0x1D4 | `m_SetSamplerStateCall[20]` | |
| 0x224 | `m_GetRenderStateCall[101]` | |
| 0x3B8 | `m_GetSamplerStateCall[20]` | |
| 0x480 | fetch constants `[32][24 bytes]` (texture fetch s at 0x480+24s; stream s uses 8-byte vertex fetch at 0x778−8s) | SetTexture / SetStreamSource |
| 0x780 | VS float constants c0..c255 (float4) → GPU reg 0x4000 | flush in 82580918 (`addi r6,r31,1920; li r5,16384`) |
| 0x1780 | PS float constants c0..c255 → GPU reg 0x4400 | `addi r6,r31,6016; li r5,17408` |
| 0x2780 / 0x2790 | VS / PS bool constants (4 dwords each) | SetVS/PSConstantB |
| 0x27A0 / 0x27E0 | VS / PS int constants (16 dwords each) | SetVS/PSConstantI |
| 0x2820 | clip planes [6] float4 | SetClipPlane |
| 0x2908..0x291C | PA_CL_VPORT_{X,Y,Z}{SCALE,OFFSET} shadow | SetViewportF |
| 0x2920.. | render-register shadow block flushed as range 0x2180.. (822F64C0) | draw prologue |
| 0x2934 | RB_DEPTHCONTROL shadow (Z enable bit1, write, func, stencil) | ZENABLE/ZFUNC/STENCIL* setters |
| 0x2948 | PA_SU_SC_MODE_CNTL shadow (cull bits 0..2, fill, poly offset) | CULLMODE/FILLMODE/DEPTHBIAS |
| 0x2940 | RB_COLORCONTROL-group (HiStencil bits) | HISTENCIL* |
| 0x2A88 | owning thread | Acquire/ReleaseThreadOwnership |
| 0x2A90 | ring RPtr write-back pointer | MakeSpace |
| 0x2A9C / 0x2AA0 | current fence / fence mask for deferred releases | Set*Shader, SetIndices |
| 0x2ABC..0x2ABF | status bytes (0x2ABC bit7 = "no real GPU"/replay, bit5/6 tiling active; 0x2ABD PIX bits) | many |
| 0x2E24 | current vertex declaration | SetVertexDeclaration |
| 0x3088 | current FVF | SetFVF |
| 0x308C | current index buffer | SetIndices / DrawIndexedVertices |
| 0x3090..0x309C | render targets [4] | SetRenderTarget, Resolve (`addi r11,r14,3108`) |
| 0x30A0 | depth-stencil surface | SetDepthStencilSurface, Resolve |
| 0x30A4 | stream vertex buffers [16] | SetStreamSource |
| 0x30E8 | stream strides (bytes /4) [16] | SetStreamSource |
| 0x30F8 | textures [26] | SetTexture, ResetAllState loops 26 samplers |
| 0x3160..0x3174 | viewport X, Y, W, H, MinZ, MaxZ (floats) | SetViewportF |
| 0x317C | scissor RECT | SetScissorRect |
| 0x318C / 0x3190 | vertex shader / pixel shader | Set*Shader |
| 0x319C / 0x31A0 | tiling predication state | SetPredication |
| 0x31A4 | predication select (bin mask) | SetPredication |
| 0x31B0..0x31C0 | SetSurfaces (tiling) RT/DS copies | SetSurfaces |
| 0x34BC / 0x34C0 | deferred-release queue (write ptr, limit; 8-byte {obj>>2, fence} entries) | Set*Shader / SetIndices / SetStreamSource |
| 0x3474 | BeginVertices saved ring ptr | EndVertices |
| 0x3A18 / 0x3A1C | primary ring base / size mask | KickOff |
| 0x5404 | PIX capture recorder (normally NULL) | 822ED7D8 |
| 0x5490 | GPU interrupt/callback fn | KickOff |
| 0x56F8 | creation flags | CreateDevice |

### Resources (same header as reblue's `d3d.h`: Common, RefCount, Fence, ReadFence, Identifier, BaseFlush)

| Object | Size | Fields |
|---|---:|---|
| D3DBaseTexture | 52 | +0x18 MipFlush, **+0x1C..+0x30 GPUTEXTURE_FETCH_CONSTANT (6 dwords)** copied by SetTexture into dev fetch slot |
| D3DSurface (RT/DS) | ≥0x24 | +0x1C surface info (→ RB_*_INFO shadow dev+0x2884/0x2888), +0x20 (→ dev+0x2940 for DS); Resolve reads the *dest texture's* fetch words at +0x24/+0x28/+0x30 |
| D3DVertexBuffer | 32 | Common = 0x00100001/0x00300001 (type 1), +0x18 = phys addr \| 3 (vertex fetch dword0), +0x1C = size \| endian (0x10000002) |
| D3DIndexBuffer | 32 | +0x18 base address, +0x1C size; Common bit31 = 32-bit indices |
| D3DVertexDeclaration | 56+12n | Common 0x00100005, +0x18 element count, +0x1C max stream, +0x20..0x2F stream remap bytes, +0x34 D3DVERTEXELEMENT9[n] (12 B each, copied verbatim) |
| D3DVertexShader / PixelShader | 40+ucode (+832 for one kind) | +0x28 microcode copy; VS has fetch-patch table (822E7EF0) |

## 5. Game renderer layer (hook candidates above the API)

| Address | Proposed name | Role | Evidence | Conf | Hook |
|---|---|---|---|---:|---:|
| 825329A8 | Renderer::Init | CreateDevice, ring 0x40000/0xA00000, front buffer 0x82C81A68, creates vertex decls (8254FB38) and sub-systems | | H | 3 |
| 824D3870 | render-thread frame | acquire device, SetShaderGPRAllocation, scene, Present 82533178, release | indirect only | M | 4 |
| 824F1E70 | **RenderPassRunner** | loops 29 entries of table 0x82A25A70, GPU timer + name marker, calls pass fn(ctx=0x82C278C8) | loop `cmpwi r29,29`, `addi r31,r31,16`, `bctrl` | H | 5 |
| 82533178 | **Renderer::Present** | Resolve to front buffer + Swap | | H | 5 |
| 824F1110 | **BatchRenderer::Render(list, mode, flag)** | walks 20-byte batch entries, per-batch predication, prepares draw records, dispatches 8254D030 | | H | 5 |
| 8254D030 | batch draw dispatch | jump table 0x82A27AB4[type] | | H | 4 |
| 82534758 | **Material/EffectPass::Begin** | SetPixelShader(pass+0x48), SetVertexShader(pass+0x4C), sets up constant tables; 52 callers | | H | 5 |
| 82534F10 | **Effect::CommitChanges** | AND dirty masks, tail-calls D3DX state manager 825B6D58 (SetTexture, VS/PS consts F/B/I) ; 67 call sites in 48 funcs, always right before draws | | H | 5 |
| 825B6D58 / 825B2F28 | D3DX effect state-manager flush | direct writes to dev+0x780 / 0x1780 | constwriters.py | H | 4 |
| 8253DCE8..8253E210 | 17 blend-mode presets | each sets ALPHABLENDENABLE/SRC/DEST/BLENDOP | | H | 3 |
| 8253E268 | SetBlendMode(ctx, mode) cached | 37 callers | | M | 3 |
| 8253E2F8 | reset default render state | | | M | 2 |
| 82541100, 82542100 (stencil two-sided, HiStencil), 825429F0, 82542C70, 82542FB8, 82543448, 825438E0, 82543DC8, 82544118, 82544388, 82536830, 825370D8, 82538ED0, 825478E8, 82547C78, 82548468, 825485F8 | object Draw() bodies (vtable slot +0x1C, e.g. vtables at 0x820130B0 / 0x82134300) | SetStreamSource+SetIndices+SetVertexDeclaration+DrawIndexedVertices | | M | 4 |
| 82546C40 | cCandyRendererXbox (UI/HUD) draw | "Render:cCandyUI:cCandyRendererXbox" | | M | 4 |
| 82551030 | Bloom::Render | "bloom_intensity"; 8× Resolve + 8× DrawVertices | effect object {Init,Release,Render,…} at 0x8213BCF0 | H | 4 |
| 82553CB0 | ToneMapAndBloomComposite::Render | | 0x8213C0E0 | H | 4 |
| 825547A8 | DepthOfField::Render | "DepthOfField_Far_Point0" | 0x8213C1F0 | H | 4 |
| 82552830 | HSV/desaturation colour pass | "HSV_PreScale" | 0x8213BE7C | M | 4 |
| 82552CB8 | Outline / Painter's-edge pass | "Outline_Scale","edgeColor" | 0x8213BFD4 | M | 4 |
| 82553890 | auto-exposure (average luminance) | "auto_average_luminance" | 0x8213C0B4 | M | 3 |
| 8254D6A8 | "PREDICATED TEST" pass | | 0x8213BB40 | M | 2 |

### Render passes (table 0x82A25A70, run by 824F1E70)

| # | Pass | fn | Notes |
|---:|---|---|---|
| 0 | Gather Batches | 824F1848 | CPU culling (824FA9F0) |
| 1 | Begin Game Render | 824F20D8 | default states (DEPTHBIAS, HALFPIXELOFFSET, FILLMODE) |
| 2 | Render Level Heightmap | 824F1850 | |
| 3 | Render Shadow Maps | 824F1880 | GPR alloc, COLORWRITEENABLE, shadow batch lists |
| 4 | Calc Light Parameters | 824F1A38 | CPU |
| 5 | Render Reflections | 824F19E8 | |
| 6 | Print Debug Info | nop | |
| 7 | Begin Tiling | 824F1A40 | → BeginTiling/SetSurfaces (predicated tiling, EDRAM) |
| 8 | Render Depth Only | 824F1AD0 | Z pre-pass |
| 9 | Render Decal | 824F1B38 | |
| 10 | Render Character ID | 824F1B88 | |
| 11 | Render Only Casts Shadows | (disabled) | |
| 12 | Deferred Shade | 824F21D8 | |
| 13 | Render Opaque | 824F1BE8 | batch list 0x82A25A68 |
| 14 | Render Opaque Character | 824F1C38 | |
| 15 | Render Skybox | 824F1CA8 | |
| 16 | Render Black River Serpent | 824F1CB8 | |
| 17 | End Tiling | 824F1CC0 | → EndTiling (per-tile resolves) |
| 18 | Render Ocean | 824F1CB0 | |
| 19 | Copy Back Buffer | 824F1D60 | 8254B7A0 (DrawVerticesUP fullscreen) |
| 20 | Render Painters Edge | 824F1D78 | |
| 21 | Resolve HDR Texture | 824F1D80 | Resolve |
| 22 | Render Sorted | 824F1DD8 | translucent |
| 23 | End Game Render | 824F2248 | |
| 24 | Render Post Process | 824F1DE0 | post-effect objects |
| 25 | Render Debug | 824F2290 | |
| 26 | Upscale | 824F1E38 | 8254BFD8 (Resolve) |
| 27 | Render Movie | 824F1E40 | |
| 28 | Render HUD | 824F1E68 | |

## 6. Recommended hook points for a native renderer

1. **API-level (reblue/Unleashed style) — recommended first.** Hook the ~15
   functions that consume state, and read everything else from the shadowed device
   struct at draw time:
   `DrawVertices 82580918`, `DrawIndexedVertices 82580D00`, `BeginVertices 825803F8`,
   `EndVertices 82580898`, `DrawVerticesUP 825808B8`, `Resolve 822F5028`,
   `BeginTiling 822F3EF8`, `EndTiling 822F4480`, `SetPredication 822F3D88`,
   `Swap 822E8EB8`, `SetRenderTarget 822E2F48`, `SetDepthStencilSurface 822E32B0`,
   `SetSurfaces 822E3588`, plus resource creation/lock (`CreateTexture 822E5878`,
   `CreateVertexBuffer 822E9FC0`, `CreateIndexBuffer 822EA0E8`, VB/IB Lock/Unlock,
   `CreateVertexDeclaration 822E8308`, `CreateVertexShader/PixelShader 822E85D0/822E84C0`).
   Render/sampler state setters, SetTexture/SetStreamSource/SetIndices/
   SetVertexDeclaration/Set*Shader and the inlined constant writes can stay
   recompiled: they only update the device shadow (+dirty bits), which a draw hook
   reads (dev+0x480 fetch consts, +0x780/+0x1780 consts, +0x2934.. register shadows,
   +0x3090 RTs, +0x318C/0x3190 shaders, +0x2E24 decl, +0x308C IB, +0x30A4 streams).
   Hooking the draws and **not calling `__imp__`** removes all PM4 generation; the
   ring (dev+0x30) then never advances, so KickOff/VdSwap traffic disappears too
   (Swap must be hooked to present natively; BlockUntilIdle/fence waits must be
   satisfied — see OPEN_QUESTIONS Q-R5).
2. **Pass-level:** 29-entry pass table (0x82A25A70) run by 824F1E70 gives natural
   per-pass scopes, profiler zones and migration granularity (e.g. move "Render
   Post Process" or "Render HUD" first). Hook 824F1E70 or individual pass fns.
3. **Material/batch-level:** 82534758 (pass begin: VS/PS pair) + 82534F10 (commit)
   bracket every mesh draw; 824F1110 + 8254D030 is the batch loop — the right place
   for later native batching / PSO caching.
4. **Tiling:** the game uses predicated tiling (BeginTiling/EndTiling/SetPredication,
   SET_BIN_MASK). A native renderer can ignore tiling (render once at full res) if the
   hooks drop predication and treat EndTiling's resolves as plain copies.

## 7. Tooling (`tools/re/`, Python 3, no deps)

| Script | Purpose |
|---|---|
| `disdb.py build` | parse `conan-port/logs/default_full.dis` + `conan_register.cpp` into `conan-port/logs/disdb.pkl` |
| `q.py dis/callers/callees/grep ADDR` | quick disassembly / xrefs |
| `gref.py ADDR…` | resolve `lis`-based absolute data references (cache `logs/gref.pkl`) |
| `pm4scan.py` | find PM4 type-3 header constants per function |
| `apisurface.py`, `fingerprint.py LO HI`, `gamelayer.py` | library API surface, per-function `this`-field fingerprints, game→API usage map |
| `statetables.py` | decode render/sampler-state template tables (names, setters, defaults) |
| `passtable.py` | dump the game render-pass table |
| `callargs.py FUNC regs` | constant arguments at each call site |
| `constwriters.py` | find inlined SetVS/PSConstantF writers |
| `names.py` → `d3d_symbols.tsv` | recovered symbol map (address, name, static callers) for hook authoring |

## 8. Device register-shadow -> Xenos register mapping (recovered 2026-09-24)

The draw prologue (e.g. 822F15D0 / 822F5028 / 822F9728 / 825803F8 variants) flushes dirty
register groups with the generic writer `822F64C0(dev, mask64, first_reg, src_ptr_minus_4)`:
for every set bit i (MSB-first run encoding), register `first_reg + i` <- `src[i]`.
Call sites (822F1618..822F1790) give the complete layout:

| Xenos regs | device shadow | count | contents (xenos register names) |
|---|---|---:|---|
| 0x2000-0x200F | dev+0x2880 | 16 | RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1..3_INFO, ... |
| 0x2100-0x2114 | dev+0x28CC | 21 | ..., RB_COLOR_MASK 0x2104, RB_BLEND_RGBA 0x2105-8, RB_STENCILREFMASK 0x210D, RB_ALPHA_REF 0x210E, PA_CL_VPORT_* 0x210F-0x2114 |
| 0x2180-0x2184 | dev+0x2920 | 5 | SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL, ... |
| 0x2200-0x220B | dev+0x2934 | 12 | RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, RB_HIZCONTROL, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, VGT_CURRENT_BIN_ID_MIN, RB_MODECONTROL, RB_BLENDCONTROL1..3 |
| 0x2280-0x2294 | dev+0x2964 | 21 | PA_SU_POINT_SIZE/MINMAX, PA_SU_LINE_CNTL, PA_SC_LINE_STIPPLE, VGT_*, PA_SU_POLY_OFFSET_* ... |
| 0x2300-0x2325 | dev+0x29B8 | 38 | RB_SAMPLE_COUNT_CTL ... RB_COPY_* (resolve), RB_DEPTH_CLEAR, RB_COLOR_CLEAR ... |

Mask sources: m_Mask[2] bits 52-63 -> 0x2200 group, bits 47-51 -> 0x2180, bits 42-57(16) -> 0x2000,
bits 22-42(21) -> 0x2100; m_Mask[3] bits 9-29 -> 0x2280; m_Mask[4] bits 26-63 -> 0x2300.
822F6128 handles the tiled/predicated subset (dev+0x2940 in some modes).
A native draw can therefore rebuild a Xenos register view from the shadow and decode
blend/depth/stencil/raster/viewport state with the SDK's `rex::graphics::reg::*` structs.

## 9. Frame render-target graph (runtime capture cap4, jungle scene, 2026-09-24)

Surface object: +0x18 RB_SURFACE_INFO (pitch px bits 0-13, msaa bits 16-17), +0x1C RB_COLOR_INFO /
RB_DEPTH_INFO (EDRAM base bits 0-11, format bits 16-19), +0x24 size bits (width = (v>>18)+1,
height = ((v>>3)&0x7FFF)+1), +0x28 D3DFORMAT. Resolve(dev, Flags, pSrcRect, pDestTex, pDestPoint,
Level, Slice, ...): Flags&7 = source (0-3 color RT, 4 = depth), 0x100 = clear color, 0x200 = clear depth.

| Stage (pass) | Render target(s) | Resolve -> texture |
|---|---|---|
| Render Shadow Maps | depth 1024x1024 (surf pitch 1040), no color | flags 0x204 -> 4096x2048 k_24_8 shadow atlas (x2/frame) |
| Begin Tiling .. Render Decal | color 8888 1024x576 4xMSAA (2 tiles 512x576), depth D24S8 | 0x100 color -> 1024x576 8888 (two half rects); 0x14 depth -> 1024x576 k_24_8 |
| Opaque / Character / Skybox | HDR color fmt 12 (2_10_10_10_FLOAT_AS_16_16_16_16) same tiles | depth -> 1024x576 k_24_8 (character) |
| End Tiling / Resolve HDR / End Game Render | HDR color | -> 1024x576 k_16_16_16_16_EXPAND (0xFBF17000) + depth -> 0xFBC87000 |
| Render Post Process | bloom 256x144 (fmt 5 16_16_16_16), luminance 1x576 / 1x1 32_FLOAT, tonemap 8888 1024x576 | several (see cap4.json) |
| Upscale | 1024x576 8888 -> 1280x720 8888 (pitch 1280) | 1024x576 -> 0xFB807000 |
| Render HUD | 1280x720 8888 | - |
| Present | 1280x720 | -> front buffer 1280x720 8888 (0xFECB7000) then Swap |

Native plan: host RTs keyed by guest surface object at full (untiled) size, 1x MSAA initially;
resolves copy host RT regions into host textures registered by destination guest base address;
SetTexture/fetch of a registered address binds the host texture (no guest memory decode).
