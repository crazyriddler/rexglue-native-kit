# Xbox 360 XDK Direct3D internals (as recovered, XDK ~2006/2007)

Recovered from Conan (statically linked XDK D3D 9 + D3DX effects). Offsets are for that
XDK revision; verify them in your game (same revision -> same values). Full evidence:
reference/conan/docs/RENDERER_ANALYSIS.md.

## Layout of the D3D code

The D3D library usually sits in one or two blocks (Conan: 0x822DD000-0x822F9D00 core +
0x8257C000-0x82581160, an LTCG-placed block with the draw entry points). The draw
prologue ("SetPendingState") is inlined into each draw entry point: it flushes dirty
constant/register groups, then writes `VGT_INDX_OFFSET (type-0 0x2102)` and
`DRAW_INDX 0xC0012201` (auto index) / `0xC0032201` (DMA index).

## D3DDevice (0x5700 bytes)

| Offset | Field |
|---|---|
| 0x000 | u64 m_Mask[6] dirty masks: [0] VS float consts, [1] PS float consts, [2] render state/shader/decl groups, [3] fetch consts, [4] misc |
| 0x030 / 0x034 / 0x038 | command segment write pointer (last written dword, packets via `stwu`), limit, make-space threshold |
| 0x03C | refcount |
| 0x040 / 0x1D4 / 0x224 / 0x3B8 | Set/Get RenderState and SamplerState dispatch tables (101 / 20 entries), seeded from a template {Get,Set,Default} table by ResetAllState |
| 0x480 | fetch constants [32] x 24 bytes (texture s at 0x480+24s; vertex stream s = 8-byte fetch at 0x778-8s) |
| 0x780 / 0x1780 | VS / PS float constants c0-c255 (-> GPU 0x4000 / 0x4400) |
| 0x2780 / 0x2790 | VS / PS bool constants (4 dwords) |
| 0x27A0 / 0x27E0 | VS / PS int (loop) constants (16 dwords) |
| 0x2820 | clip planes [6] |
| 0x2880.. | register shadow (see mapping below) |
| 0x2A88 | owner thread; 0x2A90 RPtr write-back pointer; 0x2A9C/0x2AA0 fence |
| 0x2E24 | vertex declaration |
| 0x308C | index buffer |
| 0x3090-0x309C / 0x30A0 | render targets [4] / depth-stencil |
| 0x30A4 / 0x30E8 | stream vertex buffers [16] / strides in dwords [16] |
| 0x30F8 | textures [26] |
| 0x3160-0x3174 | viewport X, Y, W, H, MinZ, MaxZ as **floats** |
| 0x317C | scissor rect |
| 0x318C / 0x3190 | shader objects (which one is VS/PS: use the shader registry's is_vertex flag; Conan's HUD pairs contradicted the obvious order) |
| 0x31A4 | predication select |
| 0x3474 | BeginVertices saved ring pointer |
| 0x3A18 / 0x3A1C | primary ring base / mask |
| 0x5404 | PIX capture recorder (NULL unless capturing) |

### Register shadow -> Xenos registers (flushed by the generic writer (dev, mask, first_reg, src-4))

| Xenos regs | Device shadow | Contents |
|---|---|---|
| 0x2000-0x200F | dev+0x2880 | RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, RB_COLOR1..3_INFO |
| 0x2100-0x2114 | dev+0x28CC | RB_COLOR_MASK 0x2104, RB_BLEND_RGBA, RB_STENCILREFMASK 0x210D, RB_ALPHA_REF 0x210E, PA_CL_VPORT_* |
| 0x2180-0x2184 | dev+0x2920 | SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL |
| 0x2200-0x220B | dev+0x2934 | RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, RB_HIZCONTROL, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, RB_MODECONTROL, RB_BLENDCONTROL1-3 |
| 0x2280-0x2294 | dev+0x2964 | PA_SU_POINT/LINE, VGT_*, PA_SU_POLY_OFFSET_* (the poly offset values are NOT reliable in the shadow - read them from the PM4 mirror) |
| 0x2300-0x2325 | dev+0x29B8 | RB_SAMPLE_COUNT_CTL .. RB_COPY_* (resolve), RB_DEPTH_CLEAR, RB_COLOR_CLEAR |

## Resources

| Object | Layout |
|---|---|
| Texture (52 bytes) | +0x1C..+0x30 = the 6-dword GPUTEXTURE_FETCH_CONSTANT copied by SetTexture |
| Surface (RT/DS) | +0x18 RB_SURFACE_INFO (pitch bits 0-13, MSAA bits 16-17: 0/1/2 = 1/2/4 samples), +0x1C RB_COLOR_INFO / RB_DEPTH_INFO (EDRAM base bits 0-11, format bits 16-19), +0x24 size (width = (v>>18)+1, height = ((v>>3)&0x7FFF)+1), +0x28 D3DFORMAT |
| Vertex buffer (32) | +0x18 = physical address \| 3 (fetch dword0), +0x1C = size \| endian |
| Index buffer (32) | +0x18 base (**virtual**: low 29 bits, +0x1000 for the 0xE0000000 view), +0x1C size, Common bit 31 = 32-bit indices |
| Vertex declaration | +0x18 element count, +0x34 D3DVERTEXELEMENT9[n] (12 bytes: stream u16, offset u16, type u32, method, usage, usage index) |
| Shader objects | +0x28 microcode copy; VS microcode is patched by the XDK at declaration bind (vfetch) -> hash the **container** at create time, not the live ucode |

Addresses: objects often hold **virtual** addresses in the 0xA0000000/0xC0000000/
0xE0000000 physical views (0xE0000000 view is offset by 4 KB); fetch constants hold
physical ones. Normalize everything to physical before using it as a cache key.

## Command stream facts

- The XDK writes PM4 into a secondary command segment (dev+0x30 write pointer) and kicks
  it via INDIRECT_BUFFER (0xC0013F00) in the primary ring + CP_RB_WPTR (MMIO 0x7FC80714).
- Constants that never touch the device shadow:
  - shader literal ("def") constants: LOAD_ALU_CONSTANT (0xC0022F00) at shader bind from
    the shader object's physical memory (c252-255 VS, c500-511 PS in Conan);
  - GpuBeginShaderConstantF4: the game writes constants directly into the ring;
  - inline SET_CONSTANT (0x2D) packets reserved by a helper and filled by the game;
  - inline type-0 register blocks emitted by inlined helpers (e.g. SpeedTree tables).
  -> Parse the command segment between draws (PM4 mirror).
- RB_COPY_DEST_INFO/BASE are reset right after each resolve: capture them at copy-mode
  draws (RB_MODECONTROL == 6). Seen values: 0x302 (8888 8in32), 0x01000302 (swap R/B),
  0xF001 (16_16_16_16_FLOAT), 0xF202 (32_FLOAT).
- Clear: `D3DDevice_Clear(dev, Count, pRects, Flags, Color, Z, Stencil)` is an internal
  draw from PM4 templates (not the draw entry points). Flags: TARGET0-3 bits 0-3,
  ZBUFFER 0x10, STENCIL 0x20.
- Resolve flags: `Flags & 7` = source (0-3 color RT, 4 = depth), 0x100 clear color after,
  0x200 clear depth/stencil after.
- Fences: BlockOnFence(dev, fence) loops on a poll that compares a GPU-written dword
  (EVENT_WRITE_SHD target, k8in32 endian) with the fence value.
- Occlusion queries (VIZ_QUERY 0xC0002300): if the host draws nothing, results are 0 and
  the game culls everything -> report visible.
- Shader GPR allocation, HiZ/HiStencil, PIX recorder: irrelevant for native rendering.

## Xenos GPU semantics to preserve

- Pixel centers: PA_SU_VTX_CNTL pix_center = 0 -> D3D9 integer centers -> half-pixel
  viewport shift (Xenia `half_pixel_offset`).
- Screen-space draws: PA_CL_VTE_CNTL without VPORT_X_SCALE_ENA -> positions are pixels.
- EDRAM: render targets live in EDRAM tiles (80x16 samples, 64 bpp halves the width);
  different surfaces alias by EDRAM base; content persists across rebinds; MSAA 2x/4x
  layout; resolves copy EDRAM -> memory (MSAA averaged).
- Predicated tiling: passes replayed per tile with bin masks; natively render once at
  full size and treat per-tile resolves as sub-rect copies at screen position.
- Texture gamma sign -> piecewise-linear (PWL) gamma, applied after filtering.
- 0 * NaN = 0 in the Xenos ALU (games rely on it with uninitialized constant lanes).
- DC_LUT gamma ramp at scan-out.
- Rect lists: 3 vertices, the 4th is derived (longest edge = diagonal).
- Quad lists, strip-cut index 0xFFFF / 0xFFFFFFFF.
- Texture formats: k_16*_EXPAND = float16; k_24_8 depth resolves; DXT/CTX1/DXN; tiled 2D
  addressing (Xenia `XenosTextureTiledAddress2D`); cubes as 6 tiled slices 4 KB aligned.
