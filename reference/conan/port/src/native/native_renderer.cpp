#include "native_renderer.h"

#include <dxgi1_4.h>

#include <algorithm>
#include <array>
#include <immintrin.h>
#include <chrono>
#include <atomic>
#include <set>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_provider.h>

#include "shader_registry.h"
#include "texture_decode.h"

REXCVAR_DEFINE_BOOL(native_renderer, true, "Conan",
                    "Render with the native game-specific renderer (no Xenos GPU plugin, EXP-036); "
                    "false = legacy Xenos emulation");
REXCVAR_DEFINE_INT32(native_debug_spin_us, 0, "Conan",
                     "Debug: busy-wait this many microseconds in every native draw entry (timing bisection)");
REXCVAR_DEFINE_INT32(native_debug_resolve_mode, 0, "Conan",
                     "Debug bisection: 1 = resolves skip copies, 2 = resolves skip clears");
REXCVAR_DEFINE_DOUBLE(native_dump_frame_at_s, 0.0, "Conan",
                      "Debug: at this bench time, read back every native surface and resolve "
                      "texture into native_dump_dir (.raw; tools/native_dump_to_png.py)");
REXCVAR_DEFINE_INT32(native_dump_after_pass, -1, "Conan",
                     "Debug: with native_dump_frame_at_s, dump at the end of this pass instead of "
                     "at the end of the frame");
REXCVAR_DEFINE_STRING(native_dump_dir, "", "Conan", "Directory for native_dump_frame_at_s");
REXCVAR_DEFINE_DOUBLE(native_trace_frame_at_s, 0.0, "Conan",
                      "Debug: log render target binds, clears and resolves of one frame after "
                      "this many seconds");
REXCVAR_DEFINE_STRING(native_pass_mask, "", "Conan",
                      "Debug: comma-separated pass indices whose draws are rendered natively "
                      "(empty = all). Pass 0 = draws outside any pass.");
REXCVAR_DEFINE_BOOL(native_buffer_cache_per_frame, false, "Conan",
                    "Debug: drop cached vertex/index buffers every frame (stale data bisection)");
REXCVAR_DEFINE_BOOL(native_ring_constants, true, "Conan",
                    "Debug: mirror GpuBeginShaderConstantF4 ring constants into the shadow");
REXCVAR_DEFINE_STRING(native_skip_draws, "", "Conan",
                      "Debug: skip draws <pass>:<first>-<last> (per-frame draw index within the pass)");
REXCVAR_DEFINE_INT32(native_dump_swap, 0, "Conan",
                     "Debug: dump the frame presented by this guest swap number (see "
                     "bench_screenshot_swaps)");
REXCVAR_DEFINE_BOOL(native_pm4_mirror, true, "Conan",
                    "Take shader/fetch constants from the parsed command stream (GPU truth) "
                    "instead of the XDK device shadow");
REXCVAR_DEFINE_BOOL(native_fixed16_snorm, false, "Conan",
                    "Experiment: store EDRAM k_16_16_16_16 render targets as SNORM (clamp to "
                    "[-1, 1]) instead of float");
REXCVAR_DEFINE_INT32(native_exp_alias_depth_base, -1, "Conan",
                     "Experiment: clear the depth surface at this EDRAM base at its first bind "
                     "each frame");
REXCVAR_DEFINE_DOUBLE(native_exp_alias_depth_value, 0.0, "Conan",
                      "Experiment: depth value for native_exp_alias_depth_base");
REXCVAR_DEFINE_STRING(native_debug_ps_const, "", "Conan",
                      "Debug: override a pixel shader constant, <ps hash>:<reg>:<x>,<y>,<z>,<w>");
REXCVAR_DEFINE_BOOL(native_edram_aliasing, true, "Conan",
                    "Clear host surfaces whose EDRAM range was overwritten by another surface "
                    "since their last use (no cross-frame feedback through aliased EDRAM)");
REXCVAR_DEFINE_BOOL(native_tile_resolve_in_place, true, "Conan",
                    "Tiling resolves with a (0,0) destination into a full-screen texture keep "
                    "the tile at its screen position");
REXCVAR_DEFINE_STRING(native_dump_texture_addr, "", "Conan",
                      "Debug: write the decoded texture at this physical base (hex) as DDS into "
                      "native_dump_dir whenever it is (re)created");
REXCVAR_DEFINE_BOOL(native_gamma_ramp, true, "Conan",
                    "Apply the guest display gamma ramp when presenting the front buffer");
REXCVAR_DEFINE_INT32(native_trace_swap, 0, "Conan",
                     "Debug: trace the frame presented by this guest swap number");
REXCVAR_DEFINE_BOOL(native_debug_no_present, false, "Conan",
                    "Debug: render natively but never refresh the presenter's guest output");
REXCVAR_DEFINE_BOOL(native_own_queue, true, "Conan",
                    "Native renderer submits on its own D3D12 direct queue");
REXCVAR_DEFINE_STRING(native_skip_ps, "", "Conan",
                      "Debug: skip draws using this pixel shader (hex container hash)");
REXCVAR_DEFINE_DOUBLE(native_edram_alias_alpha, 0.0, "Conan",
                      "Alpha written into color surfaces whose EDRAM was clobbered by aliasing");
REXCVAR_DEFINE_BOOL(native_edram_reinterpret, true, "Conan",
                    "Surfaces sharing an EDRAM base with a newer surface of another 32bpp "
                    "format take its bits (reinterpreted) instead of keeping stale contents");
REXCVAR_DEFINE_BOOL(native_texture_watch, true, "Conan",
                    "Revalidate textures only after guest writes (page write watches) instead "
                    "of hashing them every frame");
REXCVAR_DEFINE_INT32(native_texture_gamma, 2, "Conan",
                     "Xenos gamma textures (fetch sign = GAMMA): 0 = raw, 1 = sRGB views, "
                     "2 = Xenos piecewise-linear curve in the shader (descriptor index bit 31)");
REXCVAR_DEFINE_BOOL(native_worker, true, "Conan",
                    "Record D3D12 commands on a worker thread (guest threads only capture state; "
                    "requires native_pm4_mirror)");
REXCVAR_DEFINE_BOOL(native_worker_lag, true, "Conan",
                    "Let the recording worker finish a frame while the guest starts the next one");
REXCVAR_DEFINE_BOOL(native_async_pipelines, true, "Conan",
                    "Pipelines missing from the cache compile in the background for passes the game "
                    "redraws every frame (scene geometry, shadows, transparents): the draw is skipped "
                    "for a few frames instead of stalling. Other passes wait (EXP-053)");
REXCVAR_DEFINE_BOOL(native_alpha_to_coverage, true, "Conan",
                    "Xenos alpha to mask (RB_COLORCONTROL) as host alpha to coverage on MSAA targets");
REXCVAR_DEFINE_INT32(native_debug_vb_addr, 0, "Conan",
                     "Debug: log plans and first vertices of draws using this vertex buffer base");
REXCVAR_DEFINE_BOOL(native_half_pixel_offset, true, "Conan",
                    "Emulate Xenos D3D pixel centers (PA_SU_VTX_CNTL) with a half-pixel viewport shift");
REXCVAR_DEFINE_STRING(native_ab_swaps, "", "Conan",
                      "A/B: comma-separated guest swap numbers whose native output is dumped "
                      "(s<swap>_output_1280x720.raw in native_dump_dir)");
REXCVAR_DEFINE_BOOL(native_scissor, true, "Conan",
                    "Apply the guest window/screen scissor (outside predicated tiling)");
REXCVAR_DEFINE_INT32(native_debug_texture_format, -1, "Conan",
                     "Debug: log every created texture of this Xenos format");
REXCVAR_DEFINE_BOOL(native_debug_buffers_always_dirty, false, "Conan",
                    "Debug: re-upload every draw's vertex/index range (ignores Unlock tracking)");
REXCVAR_DEFINE_BOOL(native_debug_clears, false, "Conan", "Debug: log resolve depth clears");
REXCVAR_DEFINE_BOOL(native_debug_no_ztest, false, "Conan", "Debug: disable all depth tests");
REXCVAR_DEFINE_DOUBLE(render_scale, 1.0, "Graphics",
                      "Internal render resolution multiplier of 720p, may be fractional (1 = 720p, "
                      "1.5 = 1080p, 2 = 1440p, 3 = 2160p, 4 = 2880p)")
    .range(1.0, 4.0);
REXCVAR_DEFINE_INT32(shadow_quality, 1, "Graphics",
                     "Shadow map resolution multiplier (1 = original 1024, 2 = 2048, 4 = 4096)")
    .range(1, 4);
REXCVAR_DEFINE_INT32(msaa_samples, 0, "Graphics",
                     "MSAA samples for the surfaces the game multisamples (0 = game's 4x, "
                     "1 = off, 2, 4, 8; launcher: Off / 4x / 8x)")
    .range(0, 8);
REXCVAR_DEFINE_BOOL(full_scene_resolution, true, "Graphics",
                    "Render the 3D scene at the output resolution instead of the game's 1024x576 "
                    "(x render scale) stretched to 1280x720");
REXCVAR_DEFINE_BOOL(foliage_antialiasing, false, "Graphics",
                    "Alpha-tested foliage/grilles use alpha-to-coverage with the game's MSAA "
                    "(antialiased, mip-corrected leaf edges) instead of a hard alpha test");
REXCVAR_DEFINE_BOOL(ambient_occlusion, false, "Graphics",
                    "Screen-space ambient occlusion on the 3D scene");
REXCVAR_DEFINE_BOOL(ssao_debug, false, "Conan", "Debug: replace the scene with the SSAO term");
REXCVAR_DEFINE_DOUBLE(ssao_radius, 0.5, "Graphics", "SSAO sampling radius in world units");
REXCVAR_DEFINE_DOUBLE(ssao_intensity, 0.8, "Graphics", "SSAO strength (0-1)");
REXCVAR_DEFINE_DOUBLE(ssao_fade_distance, 40.0, "Graphics",
                      "SSAO fades out towards this distance from the camera (world units)");
REXCVAR_DEFINE_BOOL(soft_particles, false, "Graphics",
                    "Soft particles: blended effects fade near the geometry behind them");
REXCVAR_DEFINE_DOUBLE(soft_particle_distance, 0.4, "Graphics",
                      "Soft particle fade distance in world units");
REXCVAR_DEFINE_INT32(shadow_smoothing, 0, "Graphics",
                     "Shadow edge smoothing: 0 = off (original), 1 = on (per-pixel rotated PCF), "
                     "2 = softer (wider kernel)")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(smooth_effects, true, "Graphics",
                    "Above the original resolution, magnify small textures of blended effects "
                    "(particles, glows) with a smooth cubic filter instead of bilinear");
REXCVAR_DEFINE_BOOL(fxaa, false, "Graphics",
                    "FXAA post-process antialiasing on the 3D image (the HUD stays sharp)");
REXCVAR_DEFINE_INT32(bloom_quality, 1, "Graphics",
                     "Resolution multiplier of small post-process targets (bloom): 1 = original, 2, 4")
    .range(1, 4);
REXCVAR_DEFINE_INT32(anisotropic_filtering, -1, "Graphics",
                     "Anisotropic filtering for filtered textures (-1 = game, 1 = off, 2, 4, 8, 16)")
    .range(-1, 16);
REXCVAR_DEFINE_INT32(native_debug_rs_salt, 0, "Conan",
                     "Debug: add an unused root constant block of N dwords (1-32) so every "
                     "pipeline misses the driver's shader cache (cold-start stutter tests)");
REXCVAR_DEFINE_BOOL(native_debug_disable_driver_shader_cache, false, "Conan",
                    "Debug: disable the driver's shader cache (simulates a first run; needs "
                    "Windows developer mode)");
REXCVAR_DEFINE_BOOL(native_pipeline_cache, true, "Conan",
                    "Precompile the known pipelines at startup and record new ones "
                    "(conan_pipelines.bin) against shader-compilation stutter");
REXCVAR_DEFINE_INT32(native_shadow_pcf_mode, 0, "Conan",
                     "Debug: shadow atlas PCF at shadow_quality > 1 (0 = kernel on the host texel "
                     "grid, 1 = EXP-038 guest-texel kernel)");
REXCVAR_DEFINE_BOOL(native_gpu_breadcrumbs, false, "Conan",
                    "Debug: GPU hang breadcrumbs (a pipeline-draining marker write after every "
                    "draw; enable only to diagnose GPU hangs)");
REXCVAR_DEFINE_BOOL(native_gpu_pass_timing, false, "Conan",
                    "Debug: log GPU time per render pass (timestamp queries at pass changes)");
REXCVAR_DEFINE_BOOL(native_msaa, true, "Conan",
                    "Create host render targets with the guest's MSAA sample count");
REXCVAR_DEFINE_BOOL(native_ab_mode, false, "Conan",
                    "A/B validation: Xenos keeps rendering and presenting; the native renderer "
                    "renders the same frames offscreen (dumps only)");
REXCVAR_DEFINE_BOOL(native_flip_winding, false, "Conan", "Debug: invert front-face winding");
REXCVAR_DEFINE_BOOL(native_draws, true, "Conan", "Native renderer: issue draws (debug bisection)");
REXCVAR_DEFINE_BOOL(native_resolves, true, "Conan", "Native renderer: perform resolves (debug bisection)");
REXCVAR_DEFINE_STRING(native_shader_dir, "", "Conan",
                      "Directory with the offline shader corpus DXIL (<hash>.vs.dxil / .ps.dxil); "
                      "default: <exe>/../../../../artifacts/shaders/dxil");

namespace conan::native {

using Microsoft::WRL::ComPtr;
namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

int g_current_pass = 0;
int g_guest_pass = 0;

namespace {

constexpr uint32_t kDevicePtrAddr = 0x82C81A64;
// D3DDevice layout (docs/RENDERER_ANALYSIS.md sections 4 and 8).
constexpr uint32_t kDevFetchConstants = 0x480;  // [32] x 24 bytes
constexpr uint32_t kDevVsConstants = 0x780;     // 256 float4
constexpr uint32_t kDevPsConstants = 0x1780;    // 256 float4
constexpr uint32_t kDevVsBools = 0x2780;        // 4 dwords
constexpr uint32_t kDevPsBools = 0x2790;        // 4 dwords
constexpr uint32_t kDevVsLoops = 0x27A0;        // 16 dwords
constexpr uint32_t kDevPsLoops = 0x27E0;        // 16 dwords
constexpr uint32_t kDevShaderA = 0x318C;
constexpr uint32_t kDevShaderB = 0x3190;
constexpr uint32_t kDevVertexDecl = 0x2E24;
constexpr uint32_t kDevViewport = 0x3160;  // X, Y, W, H (u32), MinZ, MaxZ (f32)
constexpr uint32_t kDevTextures = 0x30F8;  // [26] texture objects
constexpr uint32_t kDevRenderTargets = 0x3090;  // [4] surfaces
constexpr uint32_t kDevDepthStencil = 0x30A0;
// XDK D3DDevice_Clear flags: D3DCLEAR_TARGET0..3 = bits 0-3, ZBUFFER 0x10,
// STENCIL 0x20 (the game passes 0xF = color only and 0x3F = everything).
constexpr uint32_t kClearZBuffer = 0x10;
constexpr uint32_t kClearStencil = 0x20;

// Register shadow -> Xenos register (section 8).
uint32_t RegShadowOffset(uint32_t reg_index) {
  struct Range {
    uint32_t first, count, offset;
  };
  static constexpr Range kRanges[] = {{0x2000, 16, 0x2880}, {0x2100, 21, 0x28CC},
                                      {0x2180, 5, 0x2920},  {0x2200, 12, 0x2934},
                                      {0x2280, 21, 0x2964}, {0x2300, 38, 0x29B8}};
  for (const Range& r : kRanges) {
    if (reg_index >= r.first && reg_index < r.first + r.count) {
      return r.offset + 4 * (reg_index - r.first);
    }
  }
  return 0;
}

// Guest addresses appear both as physical (device fetch slots) and as the
// 0xA0000000-0xFFFFFFFF physical views (texture objects, surfaces). Normalize
// to physical: low 29 bits, +4 KB for the 0xE0000000 view (XDK convention).
inline uint32_t GuestPhysical(uint32_t address) {
  return (address & 0x1FFFFFFFu) + (address >= 0xE0000000u ? 0x1000u : 0u);
}

// Guest memory captured with the worker command being executed (empty on the
// guest threads and in direct mode): reads of captured ranges see the guest
// state at capture time instead of the live memory.
thread_local const CaptureRange* t_capture_ranges = nullptr;
thread_local uint32_t t_capture_count = 0;
thread_local const uint8_t* t_capture_bytes = nullptr;

inline const uint8_t* GuestPtr(uint8_t* base, uint32_t addr, uint32_t len) {
  for (uint32_t i = t_capture_count; i-- > 0;) {
    const CaptureRange& r = t_capture_ranges[i];
    uint32_t d = addr - r.address;
    if (d < r.length && len <= r.length - d) return t_capture_bytes + r.offset + d;
  }
  return base + addr;
}
inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, addr, 4), 4);
  return __builtin_bswap32(v);
}
inline uint32_t Load16(uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, GuestPtr(base, addr, 2), 2);
  return __builtin_bswap16(v);
}
inline uint32_t Load8(uint8_t* base, uint32_t addr) { return *GuestPtr(base, addr, 1); }

// Texture fetch constant with RGB sign = GAMMA (dword 0 bits 2-7).
inline bool IsGammaFetch(const uint32_t fetch[6]) {
  return ((fetch[0] >> 2) & 3) == 3 && ((fetch[0] >> 4) & 3) == 3 && ((fetch[0] >> 6) & 3) == 3;
}
inline float LoadF32(uint8_t* base, uint32_t addr) {
  uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
uint32_t LoadReg(uint8_t* base, uint32_t dev, uint32_t reg_index) {
  uint32_t off = RegShadowOffset(reg_index);
  return off ? Load32(base, dev + off) : 0;
}

// Xbox D3DDECLTYPE -> DXGI and the swap unit of its guest (big-endian) data.
struct DeclFormat {
  DXGI_FORMAT format;
  uint32_t size;
  uint32_t swap;  // bytes per swapped word
};
DeclFormat MapDeclType(uint32_t type) {
  switch (type) {
    case 0x2C83A4: return {DXGI_FORMAT_R32_FLOAT, 4, 4};
    case 0x2C23A5: return {DXGI_FORMAT_R32G32_FLOAT, 8, 4};
    case 0x2A23B9: return {DXGI_FORMAT_R32G32B32_FLOAT, 12, 4};
    case 0x1A23A6: return {DXGI_FORMAT_R32G32B32A32_FLOAT, 16, 4};
    case 0x182886: return {DXGI_FORMAT_B8G8R8A8_UNORM, 4, 4};  // D3DCOLOR
    case 0x1A2286:
    case 0x1A2386: return {DXGI_FORMAT_R8G8B8A8_UINT, 4, 4};
    case 0x1A2086:
    case 0x1A2186: return {DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4};
    case 0x2C2359: return {DXGI_FORMAT_R16G16_SINT, 4, 2};
    case 0x1A235A: return {DXGI_FORMAT_R16G16B16A16_SNORM, 8, 2};
    case 0x2C2159: return {DXGI_FORMAT_R16G16_SNORM, 4, 2};
    case 0x1A215A: return {DXGI_FORMAT_R16G16B16A16_SNORM, 8, 2};
    case 0x2C2059: return {DXGI_FORMAT_R16G16_UNORM, 4, 2};
    case 0x1A205A: return {DXGI_FORMAT_R16G16B16A16_UNORM, 8, 2};
    case 0x2C82A1: return {DXGI_FORMAT_R32_UINT, 4, 4};
    case 0x2A2190:
    case 0x2A2390: return {DXGI_FORMAT_R32_UINT, 4, 4};
    case 0x2C235F: return {DXGI_FORMAT_R16G16_FLOAT, 4, 2};
    case 0x1A2360: return {DXGI_FORMAT_R16G16B16A16_FLOAT, 8, 2};
    default: return {DXGI_FORMAT_UNKNOWN, 0, 4};
  }
}

const char* UsageSemantic(uint32_t usage) {
  switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 8: return "TESSFACTOR";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    case 13: return "SAMPLE";
    default: return "TEXCOORD";
  }
}

D3D12_BLEND MapBlend(xenos::BlendFactor f) {
  using BF = xenos::BlendFactor;
  switch (f) {
    case BF::kZero: return D3D12_BLEND_ZERO;
    case BF::kOne: return D3D12_BLEND_ONE;
    case BF::kSrcColor: return D3D12_BLEND_SRC_COLOR;
    case BF::kOneMinusSrcColor: return D3D12_BLEND_INV_SRC_COLOR;
    case BF::kSrcAlpha: return D3D12_BLEND_SRC_ALPHA;
    case BF::kOneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
    case BF::kDstColor: return D3D12_BLEND_DEST_COLOR;
    case BF::kOneMinusDstColor: return D3D12_BLEND_INV_DEST_COLOR;
    case BF::kDstAlpha: return D3D12_BLEND_DEST_ALPHA;
    case BF::kOneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    case BF::kConstantColor: return D3D12_BLEND_BLEND_FACTOR;
    case BF::kOneMinusConstantColor: return D3D12_BLEND_INV_BLEND_FACTOR;
    case BF::kConstantAlpha: return D3D12_BLEND_BLEND_FACTOR;
    case BF::kOneMinusConstantAlpha: return D3D12_BLEND_INV_BLEND_FACTOR;
    case BF::kSrcAlphaSaturate: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}
D3D12_BLEND MapBlendAlpha(xenos::BlendFactor f) {
  D3D12_BLEND b = MapBlend(f);
  switch (b) {
    case D3D12_BLEND_SRC_COLOR: return D3D12_BLEND_SRC_ALPHA;
    case D3D12_BLEND_INV_SRC_COLOR: return D3D12_BLEND_INV_SRC_ALPHA;
    case D3D12_BLEND_DEST_COLOR: return D3D12_BLEND_DEST_ALPHA;
    case D3D12_BLEND_INV_DEST_COLOR: return D3D12_BLEND_INV_DEST_ALPHA;
    default: return b;
  }
}
D3D12_BLEND_OP MapBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract: return D3D12_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return D3D12_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return D3D12_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}
D3D12_TEXTURE_ADDRESS_MODE MapClamp(uint32_t clamp) {
  switch (clamp) {
    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3:
    case 5:
    case 7: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  }
}

}  // namespace

namespace {
// Shader pack embedded in conan.exe (RCDATA 2, tools/shaders/pack_shaders.py):
// 'CNSH', version, count, then {u64 hash, u32 stage, u32 offset, u32 size}
// sorted by (hash, stage), then the DXIL blobs.
// Table entries are 20 bytes, unaligned: read them with memcpy.
constexpr size_t kPackEntrySize = 20;
struct PackEntry {
  uint64_t hash;
  uint32_t stage, offset, size;
};
struct ShaderPack {
  const uint8_t* base = nullptr;
  uint32_t count = 0;
  PackEntry Entry(uint32_t i) const {
    const uint8_t* e = base + 12 + size_t(i) * kPackEntrySize;
    PackEntry r;
    std::memcpy(&r.hash, e, 8);
    std::memcpy(&r.stage, e + 8, 4);
    std::memcpy(&r.offset, e + 12, 4);
    std::memcpy(&r.size, e + 16, 4);
    return r;
  }
};
const ShaderPack& EmbeddedShaderPack() {
  static const ShaderPack pack = [] {
    ShaderPack p;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(2), MAKEINTRESOURCEW(10));  // RT_RCDATA
    if (!res) return p;
    HGLOBAL handle = LoadResource(module, res);
    const uint8_t* data = handle ? static_cast<const uint8_t*>(LockResource(handle)) : nullptr;
    DWORD size = SizeofResource(module, res);
    if (!data || size < 12 || std::memcmp(data, "CNSH", 4) != 0) return p;
    uint32_t count;
    std::memcpy(&count, data + 8, 4);
    if (12 + uint64_t(count) * kPackEntrySize > size) return p;
    p.base = data;
    p.count = count;
    return p;
  }();
  return pack;
}
}  // namespace

namespace {

D3D12_COMPARISON_FUNC MapCompare(xenos::CompareFunction f) {
  switch (uint32_t(f)) {
    case 0: return D3D12_COMPARISON_FUNC_NEVER;
    case 1: return D3D12_COMPARISON_FUNC_LESS;
    case 2: return D3D12_COMPARISON_FUNC_EQUAL;
    case 3: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case 4: return D3D12_COMPARISON_FUNC_GREATER;
    case 5: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case 6: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    default: return D3D12_COMPARISON_FUNC_ALWAYS;
  }
}
D3D12_STENCIL_OP MapStencilOp(xenos::StencilOp op) {
  switch (uint32_t(op)) {
    case 0: return D3D12_STENCIL_OP_KEEP;
    case 1: return D3D12_STENCIL_OP_ZERO;
    case 2: return D3D12_STENCIL_OP_REPLACE;
    case 3: return D3D12_STENCIL_OP_INCR_SAT;
    case 4: return D3D12_STENCIL_OP_DECR_SAT;
    case 5: return D3D12_STENCIL_OP_INVERT;
    case 6: return D3D12_STENCIL_OP_INCR;
    default: return D3D12_STENCIL_OP_DECR;
  }
}

// Xenos ColorRenderTargetFormat -> host render target format.
DXGI_FORMAT MapColorRtFormat(uint32_t f) {
  switch (f) {
    case 0:   // 8_8_8_8
    case 1:   // 8_8_8_8_GAMMA
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2:   // 2_10_10_10
    case 10:  // 2_10_10_10_AS_10_10_10_10
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3:   // 2_10_10_10_FLOAT
    case 12:  // 2_10_10_10_FLOAT_AS_16_16_16_16
    case 7:   // 16_16_16_16_FLOAT
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4:   // 16_16
      return DXGI_FORMAT_R16G16_SNORM;
    case 5:   // 16_16_16_16 (Xenos fixed -32..32; approximated as float)
      return REXCVAR_GET(native_fixed16_snorm) ? DXGI_FORMAT_R16G16B16A16_SNORM
                                               : DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 6:   // 16_16_FLOAT
      return DXGI_FORMAT_R16G16_FLOAT;
    case 14:  // 32_FLOAT
      return DXGI_FORMAT_R32_FLOAT;
    case 15:  // 32_32_FLOAT
      return DXGI_FORMAT_R32G32_FLOAT;
    default:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

inline uint64_t MakeKey(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }

// Guest-pixel rectangles -> host pixels of a surface rendered at `scale`.
// Guest pixel coordinate -> host pixel coordinate of a surface rendered at
// `scale` (possibly fractional). Every edge goes through the same rounding, so
// adjacent regions (tiles, atlas cells) still meet exactly.
inline uint32_t HostPx(uint32_t v, float scale) {
  return uint32_t(std::lround(double(v) * double(scale)));
}
inline LONG HostPx(LONG v, float scale) { return LONG(std::lround(double(v) * double(scale))); }
inline D3D12_RECT ScaleRect(D3D12_RECT r, float scale) {
  return {HostPx(r.left, scale), HostPx(r.top, scale), HostPx(r.right, scale),
          HostPx(r.bottom, scale)};
}
inline std::vector<D3D12_RECT> ScaleRects(const std::vector<D3D12_RECT>& rs, float scale) {
  std::vector<D3D12_RECT> out;
  out.reserve(rs.size());
  for (const D3D12_RECT& r : rs) out.push_back(ScaleRect(r, scale));
  return out;
}

#include "shaders/blit_ps.h"
#include "shaders/blit_vs.h"
#include "shaders/fxaa_ps.h"
#include "shaders/ssao_apply_ms_ps.h"
#include "shaders/ssao_apply_ps.h"
#include "shaders/ssao_ms_ps.h"
#include "shaders/ssao_ps.h"
#include "shaders/depth_copy_ps.h"
#include "shaders/depth_copy_ms_ps.h"
#include "shaders/depth_copy_vs.h"
#include "shaders/edram_alias_ms_ps.h"
#include "shaders/edram_alias_ps.h"
#include "shaders/edram_alias_vs.h"

}  // namespace

bool Enabled() {
  static const bool enabled = REXCVAR_GET(native_renderer);
  return enabled;
}

Renderer& Renderer::Get() {
  static Renderer renderer;
  return renderer;
}

bool PackShaderLookup(uint64_t hash, bool vertex, const void** data, size_t* size);

bool Renderer::EnsureInitialized() {
  if (initialized_) {
    return true;
  }
  if (init_failed_) {
    return false;
  }
  init_failed_ = true;
  render_scale_ = float(std::clamp(REXCVAR_GET(render_scale), 1.0, 4.0));
  shadow_scale_ = float(std::clamp(REXCVAR_GET(shadow_quality), 1, 4));
  {
    int32_t m = REXCVAR_GET(msaa_samples);
    msaa_samples_ = m >= 8 ? 8u : m >= 4 ? 4u : m >= 2 ? 2u : m == 1 ? 1u : 0u;
  }
  anisotropy_ = REXCVAR_GET(anisotropic_filtering);
  full_scene_resolution_ = REXCVAR_GET(full_scene_resolution);
  foliage_aa_ = REXCVAR_GET(foliage_antialiasing);
  fxaa_ = REXCVAR_GET(fxaa);
  smooth_effects_ = REXCVAR_GET(smooth_effects);
  shadow_smoothing_ = REXCVAR_GET(shadow_smoothing);
  soft_particles_ = REXCVAR_GET(soft_particles);
  ssao_ = REXCVAR_GET(ambient_occlusion);
  bloom_scale_ = float(std::clamp(REXCVAR_GET(bloom_quality), 1, 4));
  output_width_ = HostPx(kOutputWidth, render_scale_);
  output_height_ = HostPx(kOutputHeight, render_scale_);
  REXLOG_INFO("native: render scale {} ({}x{}), shadow scale {}, msaa {}, anisotropy {}",
              render_scale_, output_width_, output_height_, shadow_scale_, msaa_samples_,
              anisotropy_);
  if (REXCVAR_GET(native_texture_watch)) {
    page_write_seq_ = std::make_unique<std::atomic<uint32_t>[]>(0x20000);
    for (uint32_t p = 0; p < 0x20000; ++p) page_write_seq_[p].store(0);
    REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&Renderer::OnPhysicalWrite,
                                                                     this);
    texture_watch_ = true;
  }

  auto* kernel_state = REX_KERNEL_STATE();
  auto* graphics = kernel_state && kernel_state->emulator()
                       ? kernel_state->emulator()->graphics_system()
                       : nullptr;
  if (!graphics || !graphics->presenter()) {
    REXLOG_ERROR("native: no graphics system / presenter");
    return false;
  }
  presenter_ = static_cast<rex::ui::d3d12::D3D12Presenter*>(graphics->presenter());
  graphics_ = graphics;
  rex::perf::RegisterGpuSwapCallback([](uint64_t swap, const uint32_t* regs, uint32_t count) {
    Renderer::Get().OnGpuSwap(swap, regs, count);
  });
  provider_ = &presenter_->provider();
  device_ = provider_->GetDevice();
  queue_ = provider_->GetDirectQueue();
  if (REXCVAR_GET(native_own_queue)) {
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (SUCCEEDED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&own_queue_)))) {
      queue_ = own_queue_.Get();
    }
  }

  // Xenos keeps consuming PM4 (fences, interrupts, swap timing) but must not
  // translate or present anything.
  // native_ab_mode keeps full Xenos rendering + presentation alongside the
  // native renderer (same guest frame through both paths, for A/B diffs).
  if (!REXCVAR_GET(native_ab_mode)) {
    rex::cvar::SetFlagByName("gpu_null_draws", "true");
    // With Xenos drawing nothing, real occlusion queries would all report zero
    // samples and the game's visibility culling (per-batch viz queries) would
    // drop almost every draw; report the fake "visible" count instead.
    rex::cvar::SetFlagByName("occlusion_query_enable", "false");
  }

  for (auto& allocator : allocators_) {
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator)))) {
      REXLOG_ERROR("native: CreateCommandAllocator failed");
      return false;
    }
  }
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0].Get(),
                                        nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXLOG_ERROR("native: CreateCommandList failed");
    return false;
  }
  command_list_->Close();
  if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
    return false;
  }
  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);

  if (!CreateDrawResources()) {
    return false;
  }

  // With --d3d12_debug, route debug-layer messages into the log.
  ComPtr<ID3D12InfoQueue1> info_queue;
  if (SUCCEEDED(device_->QueryInterface(IID_PPV_ARGS(&info_queue)))) {
    DWORD cookie = 0;
    info_queue->RegisterMessageCallback(
        [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id,
           LPCSTR description, void*) {
          static std::atomic<int> logged{0};
          if (severity <= D3D12_MESSAGE_SEVERITY_WARNING && logged.fetch_add(1) < 200) {
            REXLOG_ERROR("native: D3D12 [{}] id {}: {}", int(severity), int(id), description);
          }
        },
        D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
    REXLOG_INFO("native: D3D12 debug message callback registered");
  }

  // Device-removal watchdog: a GPU fault in native work would otherwise show
  // up only as a guest stall (Xenos CP blocked on a dead device).
  std::thread([device = device_]() {
    for (;;) {
      Sleep(1000);
      HRESULT reason = device->GetDeviceRemovedReason();
      if (reason != S_OK) {
        REXLOG_ERROR("native: D3D12 DEVICE REMOVED, reason {:08X}", uint32_t(reason));
        rex::FlushLogging();
        return;
      }
    }
  }).detach();

  REXLOG_INFO("native: renderer initialized, shaders from {}",
              EmbeddedShaderPack().count && REXCVAR_GET(native_shader_dir).empty()
                  ? fmt::format("embedded pack ({} shaders)", EmbeddedShaderPack().count)
                  : shader_dir_);
  init_failed_ = false;
  initialized_ = true;
  return true;
}

bool Renderer::CreateDrawResources() {
  // --- Main root signature: 3 root CBVs + bindless SRV/sampler tables. ---
  D3D12_DESCRIPTOR_RANGE srv_ranges[3] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    srv_ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_ranges[i].NumDescriptors = kSrvHeapSize;
    srv_ranges[i].RegisterSpace = i;
  }
  D3D12_DESCRIPTOR_RANGE sampler_range{};
  sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  sampler_range.NumDescriptors = kSamplerHeapSize;
  sampler_range.RegisterSpace = 3;
  D3D12_ROOT_PARAMETER params[8] = {};
  for (uint32_t i = 0; i < 3; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[i].Descriptor.ShaderRegister = i;
    params[i].Descriptor.RegisterSpace = 4;
    params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3 + i].DescriptorTable.NumDescriptorRanges = 1;
    params[3 + i].DescriptorTable.pDescriptorRanges = &srv_ranges[i];
    params[3 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[6].DescriptorTable.NumDescriptorRanges = 1;
  params[6].DescriptorTable.pDescriptorRanges = &sampler_range;
  params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC rs_desc{};
  rs_desc.NumParameters = 7;
  // Debug (native_debug_rs_salt): an unused root-constant parameter makes every
  // pipeline new to the driver's shader cache - a cold first run on demand.
  if (int32_t salt = REXCVAR_GET(native_debug_rs_salt); salt > 0) {
    params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[7].Constants.ShaderRegister = 15;
    params[7].Constants.RegisterSpace = 7;
    params[7].Constants.Num32BitValues = uint32_t(std::min(salt, 32));
    params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rs_desc.NumParameters = 8;
  }
  rs_desc.pParameters = params;
  rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> blob, error;
  if (FAILED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&root_signature_)))) {
    REXLOG_ERROR("native: root signature creation failed: {}",
                 error ? static_cast<const char*>(error->GetBufferPointer()) : "?");
    return false;
  }

  // --- Blit root signature: SRV table t0 + static linear sampler. ---
  D3D12_DESCRIPTOR_RANGE blit_range{};
  blit_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  blit_range.NumDescriptors = 1;
  D3D12_ROOT_PARAMETER blit_param{};
  blit_param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  blit_param.DescriptorTable.NumDescriptorRanges = 1;
  blit_param.DescriptorTable.pDescriptorRanges = &blit_range;
  blit_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC blit_sampler{};
  blit_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  blit_sampler.AddressU = blit_sampler.AddressV = blit_sampler.AddressW =
      D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  blit_sampler.MaxLOD = D3D12_FLOAT32_MAX;
  blit_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_PARAMETER blit_params[2] = {blit_param, {}};
  blit_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  blit_params[1].Descriptor.ShaderRegister = 0;
  blit_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC blit_rs{};
  blit_rs.NumParameters = 2;
  blit_rs.pParameters = blit_params;
  blit_rs.NumStaticSamplers = 1;
  blit_rs.pStaticSamplers = &blit_sampler;
  blob.Reset();
  error.Reset();
  if (FAILED(D3D12SerializeRootSignature(&blit_rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
      FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                          IID_PPV_ARGS(&blit_root_signature_)))) {
    return false;
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC blit{};
  blit.pRootSignature = blit_root_signature_.Get();
  blit.VS = {kBlitVS, sizeof(kBlitVS)};
  blit.PS = {kBlitPS, sizeof(kBlitPS)};
  blit.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  blit.SampleMask = UINT_MAX;
  blit.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  blit.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  blit.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  blit.NumRenderTargets = 1;
  blit.RTVFormats[0] = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  blit.SampleDesc.Count = 1;
  if (FAILED(device_->CreateGraphicsPipelineState(&blit, IID_PPV_ARGS(&blit_pipeline_)))) {
    REXLOG_ERROR("native: blit PSO creation failed");
    return false;
  }

  // --- Depth resolve copy: root constants (source offset) + SRV table t0. ---
  {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_DESCRIPTOR_RANGE range1 = range;
    range1.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER dc_params[3] = {};
    dc_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    dc_params[0].Constants.Num32BitValues = 2;
    dc_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    dc_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    dc_params[1].DescriptorTable.NumDescriptorRanges = 1;
    dc_params[1].DescriptorTable.pDescriptorRanges = &range;
    dc_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    dc_params[2] = dc_params[1];
    dc_params[2].DescriptorTable.pDescriptorRanges = &range1;
    D3D12_ROOT_SIGNATURE_DESC dc_rs{};
    dc_rs.NumParameters = 3;
    dc_rs.pParameters = dc_params;
    blob.Reset();
    error.Reset();
    if (FAILED(D3D12SerializeRootSignature(&dc_rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&depth_copy_root_signature_)))) {
      return false;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC dc{};
    dc.pRootSignature = depth_copy_root_signature_.Get();
    dc.VS = {kDepthCopyVS, sizeof(kDepthCopyVS)};
    dc.PS = {kDepthCopyPS, sizeof(kDepthCopyPS)};
    dc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    dc.SampleMask = UINT_MAX;
    dc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    dc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    dc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    dc.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    dc.NumRenderTargets = 2;
    dc.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
    dc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    dc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&dc, IID_PPV_ARGS(&depth_copy_pipeline_)))) {
      REXLOG_ERROR("native: depth copy PSO creation failed");
      return false;
    }
    dc.PS = {kDepthCopyMsPS, sizeof(kDepthCopyMsPS)};
    if (FAILED(device_->CreateGraphicsPipelineState(&dc,
                                                    IID_PPV_ARGS(&depth_copy_ms_pipeline_)))) {
      REXLOG_ERROR("native: depth copy (MSAA) PSO creation failed");
      return false;
    }
  }

  {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 2;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 2;
    rs.pParameters = params;
    blob.Reset();
    error.Reset();
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&alias_root_signature_)))) {
      return false;
    }
  }

  // --- Descriptor heaps. ---
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heap_desc.NumDescriptors = kSrvHeapSize;
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_)))) {
    return false;
  }
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  heap_desc.NumDescriptors = kSamplerHeapSize;
  if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&sampler_heap_)))) {
    return false;
  }
  heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap_desc.NumDescriptors = kRtvHeapSize;
  if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap_)))) {
    return false;
  }
  heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  heap_desc.NumDescriptors = kDsvHeapSize;
  if (FAILED(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&dsv_heap_)))) {
    return false;
  }
  // SRV slot 0: null 2D texture (unbound fetch slots sample zero).
  D3D12_SHADER_RESOURCE_VIEW_DESC null_srv{};
  null_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  null_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  null_srv.Texture2D.MipLevels = 1;
  device_->CreateShaderResourceView(nullptr, &null_srv,
                                    srv_heap_->GetCPUDescriptorHandleForHeapStart());
  // Slots 1/2: null 3D and cube textures (the dimension must match the shader's heap view).
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
  null_srv.Texture3D.MipLevels = 1;
  device_->CreateShaderResourceView(
      nullptr, &null_srv,
      provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), 1));
  null_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
  null_srv.TextureCube.MipLevels = 1;
  device_->CreateShaderResourceView(
      nullptr, &null_srv,
      provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), 2));

  // --- Output render target (presenter guest output format). ---
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = output_width_;
  desc.Height = output_height_;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = rex::ui::d3d12::D3D12Presenter::kGuestOutputFormat;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = desc.Format;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                IID_PPV_ARGS(&output_rts_[i])))) {
      return false;
    }
    output_rt_states_[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    output_rtvs_[i] = provider_->OffsetRTVDescriptor(
        rtv_heap_->GetCPUDescriptorHandleForHeapStart(), rtv_heap_next_++);
    device_->CreateRenderTargetView(output_rts_[i].Get(), nullptr, output_rtvs_[i]);
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&present_allocators_[i])))) {
      return false;
    }
  }
  if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                        present_allocators_[0].Get(), nullptr,
                                        IID_PPV_ARGS(&present_list_))) ||
      FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence_)))) {
    return false;
  }
  present_list_->Close();

  // --- Per-frame upload rings. ---
  D3D12_HEAP_PROPERTIES upload_heap{};
  upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC buf{};
  buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf.Width = kUploadBytesPerFrame;
  buf.Height = 1;
  buf.DepthOrArraySize = 1;
  buf.MipLevels = 1;
  buf.SampleDesc.Count = 1;
  buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&upload_buffers_[i])))) {
      return false;
    }
    D3D12_RANGE none{0, 0};
    upload_buffers_[i]->Map(0, &none, reinterpret_cast<void**>(&upload_cpu_[i]));
  }

  shader_dir_ = REXCVAR_GET(native_shader_dir);
  if (shader_dir_.empty()) {
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    shader_dir_ = (exe_dir / ".." / ".." / ".." / ".." / "artifacts" / "shaders" / "dxil")
                      .lexically_normal()
                      .string();
  }
  if (REXCVAR_GET(native_debug_disable_driver_shader_cache)) {
    // Debug: simulate a first run (cold driver shader cache). Needs Windows
    // developer mode.
    ComPtr<ID3D12Device9> device9;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(&device9));
    if (SUCCEEDED(hr)) {
      hr = device9->ShaderCacheControl(D3D12_SHADER_CACHE_KIND_FLAG_IMPLICIT_DRIVER_MANAGED,
                                       D3D12_SHADER_CACHE_CONTROL_FLAG_DISABLE);
    }
    REXLOG_INFO("native: driver shader cache disabled: {:08X}", uint32_t(hr));
  }
  if (REXCVAR_GET(native_pipeline_cache)) {
    // Sample count the renderer uses on the surfaces the game multisamples
    // (msaa_samples 0 = the game's own 4x), and whether foliage antialiasing
    // (alpha to coverage) can apply: the cache converts captured pipelines to
    // these settings (EXP-052).
    uint32_t msaa = msaa_samples_ ? msaa_samples_ : 4;
    pipeline_cache_.Start(device_, root_signature_.Get(), &PackShaderLookup, &UsageSemantic, msaa,
                          foliage_aa_ && msaa > 1);
  }
  return true;
}

void Renderer::Retire(ComPtr<ID3D12Resource> resource) {
  if (resource) {
    // Safe once the fence value of the frame currently being recorded completes.
    retired_.emplace_back(next_fence_value_, std::move(resource));
  }
}

uint32_t Renderer::AllocSrvIndex() {
  if (!retired_srvs_.empty() && retired_srvs_.front().first <= fence_->GetCompletedValue()) {
    uint32_t index = retired_srvs_.front().second;
    retired_srvs_.erase(retired_srvs_.begin());
    return index;
  }
  return srv_heap_next_ < kSrvHeapSize ? srv_heap_next_++ : 0;
}

bool Renderer::Upload(size_t size, size_t alignment, UploadAlloc& out) {
  size_t offset = (upload_offset_ + alignment - 1) & ~(alignment - 1);
  if (offset + size <= kUploadBytesPerFrame) {
    upload_offset_ = offset + size;
    out.cpu = upload_cpu_[frame_index_] + offset;
    out.gpu = upload_buffers_[frame_index_]->GetGPUVirtualAddress() + offset;
    out.resource = upload_buffers_[frame_index_].Get();
    out.offset = offset;
    return true;
  }
  // Ring exhausted: continue in overflow pages, retired with this frame.
  offset = (overflow_offset_ + alignment - 1) & ~(alignment - 1);
  if (!overflow_page_ || offset + size > overflow_size_) {
    if (overflow_page_) Retire(overflow_page_);
    overflow_size_ = std::max<size_t>(size + alignment, 64u << 20);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = overflow_size_;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    overflow_page_.Reset();
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&overflow_page_)))) {
      overflow_size_ = 0;
      return false;
    }
    D3D12_RANGE none{0, 0};
    overflow_page_->Map(0, &none, reinterpret_cast<void**>(&overflow_cpu_));
    ++overflow_pages_created_;
    offset = 0;
  }
  overflow_offset_ = offset + size;
  out.cpu = overflow_cpu_ + offset;
  out.gpu = overflow_page_->GetGPUVirtualAddress() + offset;
  out.resource = overflow_page_.Get();
  out.offset = offset;
  return true;
}

void Renderer::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state,
                          D3D12_RESOURCE_STATES after) {
  if (state == after) {
    return;
  }
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = state;
  barrier.Transition.StateAfter = after;
  command_list_->ResourceBarrier(1, &barrier);
  state = after;
}

void Renderer::BeginFrameTimestamp() {
  if (!ts_heap_) {
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kTsPerFrame * kFramesInFlight;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 8 * kTsPerFrame * kFramesInFlight;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(queue_->GetTimestampFrequency(&ts_frequency_)) ||
        FAILED(device_->CreateQueryHeap(&qd, IID_PPV_ARGS(&ts_heap_))) ||
        FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&ts_readback_)))) {
      ts_heap_.Reset();
      return;
    }
    ts_readback_->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint64_t**>(&ts_cpu_)));
    ts_last_log_ = std::chrono::steady_clock::now();
  }
  // This frame slot's previous timestamps are complete (BeginFrame waited on
  // its fence).
  if (ts_pending_[frame_index_] && ts_cpu_ && ts_frequency_) {
    const uint64_t* t = ts_cpu_ + kTsPerFrame * frame_index_;
    uint32_t n = ts_count_[frame_index_];
    if (n >= 2 && t[n - 1] > t[0]) {
      ts_accum_ms_ += double(t[n - 1] - t[0]) * 1000.0 / double(ts_frequency_);
      ++ts_frames_;
      for (uint32_t i = 0; i + 1 < n; ++i) {
        if (t[i + 1] >= t[i]) {
          ts_pass_ms_[ts_pass_[frame_index_][i] & 31] +=
              double(t[i + 1] - t[i]) * 1000.0 / double(ts_frequency_);
        }
      }
    }
    ts_pending_[frame_index_] = false;
  }
  auto now = std::chrono::steady_clock::now();
  if (ts_frames_ && now - ts_last_log_ > std::chrono::seconds(5)) {
    REXLOG_INFO("native: GPU time {:.3f} ms/frame ({} frames)", ts_accum_ms_ / double(ts_frames_),
                ts_frames_);
    if (REXCVAR_GET(native_gpu_pass_timing)) {
      std::string split;
      for (int pass = 0; pass < 32; ++pass) {
        if (ts_pass_ms_[pass] / double(ts_frames_) >= 0.005) {
          split += fmt::format(" p{}:{:.3f}", pass, ts_pass_ms_[pass] / double(ts_frames_));
        }
      }
      REXLOG_INFO("native: GPU ms/frame per pass:{}", split);
    }
    std::fill(std::begin(ts_pass_ms_), std::end(ts_pass_ms_), 0.0);
    ts_accum_ms_ = 0.0;
    ts_frames_ = 0;
    ts_last_log_ = now;
  }
  ts_count_[frame_index_] = 0;
  ts_last_pass_ = -1;
  ts_open_ = true;
  PassTimestamp(g_current_pass);
}

void Renderer::PassTimestamp(int pass) {
  if (!ts_heap_ || !ts_open_ || pass == ts_last_pass_) return;
  uint32_t& n = ts_count_[frame_index_];
  if (n >= kTsPerFrame - 1) return;  // keep the last slot for the frame end
  ts_last_pass_ = pass;
  ts_pass_[frame_index_][n] = uint8_t(pass);
  command_list_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          kTsPerFrame * frame_index_ + n);
  ++n;
}

void Renderer::EndFrameTimestamp() {
  if (!ts_heap_ || !ts_open_) return;
  ts_open_ = false;
  uint32_t& n = ts_count_[frame_index_];
  command_list_->EndQuery(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                          kTsPerFrame * frame_index_ + n);
  ++n;
  command_list_->ResolveQueryData(ts_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                                  kTsPerFrame * frame_index_, n, ts_readback_.Get(),
                                  8 * kTsPerFrame * frame_index_);
  ts_pending_[frame_index_] = true;
}

bool Renderer::BeginFrame() {
  if (frame_open_) {
    return true;
  }
  if (!EnsureInitialized()) {
    return false;
  }
  uint64_t wait_value = frame_fence_values_[frame_index_];
  auto wait_start = std::chrono::steady_clock::now();
  struct WaitTimer {
    std::chrono::steady_clock::time_point start;
    uint64_t* total;
    ~WaitTimer() {
      *total += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count());
    }
  } wait_timer{wait_start, &stats_.gpu_wait_us};
  if (wait_value && fence_->GetCompletedValue() < wait_value) {
    fence_->SetEventOnCompletion(wait_value, fence_event_);
    if (WaitForSingleObject(fence_event_, 2000) != WAIT_OBJECT_0) {
      HRESULT reason = device_->GetDeviceRemovedReason();
      REXLOG_ERROR(
          "native: GPU fence timeout (frame {}), device removed reason {:08X}, waiting for {} "
          "completed {} next {} upload_used {}",
          frame_count_, uint32_t(reason), wait_value, fence_->GetCompletedValue(),
          next_fence_value_, upload_offset_);
      ReportHang();
      rex::FlushLogging();
      WaitForSingleObject(fence_event_, INFINITE);
    }
  }
  if (present_fence_ && present_fence_values_[frame_index_] &&
      present_fence_->GetCompletedValue() < present_fence_values_[frame_index_]) {
    present_fence_->SetEventOnCompletion(present_fence_values_[frame_index_], nullptr);
  }
  allocators_[frame_index_]->Reset();
  command_list_->Reset(allocators_[frame_index_].Get(), nullptr);
  BeginFrameTimestamp();
  upload_peak_ = std::max(upload_peak_, upload_offset_);
  upload_offset_ = 0;
  cb_vs_gpu_ = cb_ps_gpu_ = 0;
  if (overflow_page_) {
    Retire(overflow_page_);
    overflow_page_.Reset();
    overflow_offset_ = overflow_size_ = 0;
  }
  uint64_t completed = fence_->GetCompletedValue();
  retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                [completed](const auto& r) { return r.first <= completed; }),
                 retired_.end());
  frame_open_ = true;
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
  return true;
}

Renderer::HostSurface* Renderer::GetSurface(uint8_t* base, uint32_t surface_object, bool depth,
                                            uint32_t min_width, uint32_t min_height) {
  if (!surface_object) {
    return nullptr;
  }
  uint32_t surface_info = Load32(base, surface_object + 0x18);
  uint32_t color_depth_info = Load32(base, surface_object + 0x1C);
  uint32_t size_bits = Load32(base, surface_object + 0x24);
  uint32_t width = (size_bits >> 18) + 1;
  uint32_t height = ((size_bits >> 3) & 0x7FFF) + 1;
  if (tiling_active_) {
    width = std::max(width, tiling_width_);
    height = std::max(height, tiling_height_);
  }
  width = std::max(width, min_width);
  height = std::max(height, min_height);
  uint32_t format = (color_depth_info >> 16) & 0xF;
  // Guest surfaces alias by EDRAM location: different surface objects with the
  // same EDRAM base (e.g. the depth pre-pass and the opaque pass depth) share
  // contents on Xenos, so the host RT is keyed by EDRAM base + format + size,
  // not by the surface object (EXP-013). Depth keeps no format distinction.
  uint32_t edram_base = color_depth_info & 0xFFF;
  uint32_t guest_msaa = (surface_info >> 16) & 3;
  uint32_t samples = REXCVAR_GET(native_msaa) ? (guest_msaa == 2 ? 4u : guest_msaa == 1 ? 2u : 1u)
                                              : 1u;
  // msaa_samples setting: applies to the surfaces the game multisamples.
  if (samples > 1 && msaa_samples_) samples = msaa_samples_;
  uint32_t samples_log2 = samples >= 8 ? 3 : samples >= 4 ? 2 : samples >= 2 ? 1 : 0;
  uint64_t key = MakeKey((edram_base << 16) | (depth ? 0x8000u : format) | (samples_log2 << 12),
                         (width << 16) ^ height);
  auto it = surfaces_.find(key);
  if (it != surfaces_.end()) {
    return &it->second;
  }
  HostSurface& s = surfaces_[key];
  s.edram_base = edram_base;
  {
    // Footprint of the guest surface itself (per tile when tiling).
    uint32_t pitch = surface_info & 0x3FFF;
    if (!pitch) pitch = (size_bits >> 18) + 1;
    uint32_t guest_height = ((size_bits >> 3) & 0x7FFF) + 1;
    uint32_t msaa = (surface_info >> 16) & 3;
    uint32_t samples_x = msaa >= 2 ? 2 : 1, samples_y = msaa >= 1 ? 2 : 1;
    bool wide = !depth && (format == 5 || format == 7 || format == 15);
    uint32_t tile_w = wide ? 40 : 80;
    s.edram_tiles = ((pitch * samples_x + tile_w - 1) / tile_w) *
                    ((guest_height * samples_y + 15) / 16);
  }
  s.guest_format = depth ? 0xFFu : format;
  s.width = width;
  s.height = height;
  s.depth = depth;
  // Shadow map targets (square, drawn by "Render Shadow Maps") follow the
  // shadow quality setting, everything else the render resolution. The scene
  // depth is also first touched during that pass (clears), hence the shape
  // test. Reduction targets (the 1x576 / 1x1 luminance chain) stay 1:1: their
  // draws address single texels, and scaling them adds nothing visible.
  if (width < 16 || height < 16) {
    s.scale = 1.0f;
  } else if (g_current_pass == kPassRenderShadowMaps && width == height) {
    s.scale = shadow_scale_;
    s.shadow = true;
  } else {
    s.scale = render_scale_;
    // Graphics options (EXP-045). The game renders its 3D scene at 1024x576 and
    // stretches it to the 1280x720 output (pass "Upscale"): full_scene_resolution
    // renders every surface except the output-sized ones 1280/1024 larger, so
    // the scene reaches the output 1:1. Small post-process targets (the
    // 256x144 bloom chain) get bloom_quality on top.
    bool output_sized = width == kOutputWidth && height == kOutputHeight;
    if (!output_sized && full_scene_resolution_) s.scale *= float(kOutputWidth) / 1024.0f;
    if (!output_sized && width <= 512 && height <= 512) s.scale *= bloom_scale_;
  }
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = HostPx(width, s.scale);
  desc.Height = HostPx(height, s.scale);
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = samples;
  s.samples = samples;
  D3D12_CLEAR_VALUE clear{};
  if (depth) {
    s.resource_format = DXGI_FORMAT_R24G8_TYPELESS;
    s.view_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    s.srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    clear.Format = s.view_format;
    clear.DepthStencil.Depth = 1.0f;
    s.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
  } else {
    s.resource_format = MapColorRtFormat(format);
    s.view_format = s.resource_format;
    s.srv_format = s.resource_format;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    clear.Format = s.view_format;
    s.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  }
  desc.Format = s.resource_format;
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, s.state, &clear,
                                              IID_PPV_ARGS(&s.resource)))) {
    REXLOG_ERROR("native: surface creation failed {}x{} fmt {}", width, height, format);
    surfaces_.erase(key);
    return nullptr;
  }
  if (depth) {
    s.view = provider_->OffsetDSVDescriptor(dsv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            dsv_heap_next_++ % kDsvHeapSize);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = s.view_format;
    dsv.ViewDimension =
        samples > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(s.resource.Get(), &dsv, s.view);
    command_list_->ClearDepthStencilView(s.view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                         1.0f, 0, 0, nullptr);
  } else {
    s.view = provider_->OffsetRTVDescriptor(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            rtv_heap_next_++ % kRtvHeapSize);
    device_->CreateRenderTargetView(s.resource.Get(), nullptr, s.view);
    float zero[4] = {};
    command_list_->ClearRenderTargetView(s.view, zero, 0, nullptr);
  }
  REXLOG_INFO("native: surface {:08X} {}x{} x{} {} fmt {} samples {}", surface_object, width,
              height, s.scale, depth ? "depth" : "color", format, samples);
  return &s;
}

std::string Renderer::SurfaceName(const HostSurface* s) const {
  if (!s) return "-";
  return fmt::format("{}@{}:{}x{}", s->guest_format == 0xFF ? "Z" : fmt::format("f{}", s->guest_format),
                     s->edram_base, s->width, s->height);
}

namespace {
// EDRAM 32bpp encoding kind of a guest color format (edram_alias.hlsl), -1 if
// reinterpretation is not modeled.
int EdramAliasKind(uint32_t guest_format) {
  switch (guest_format) {
    case 0:
    case 1:
      return 0;  // 8_8_8_8 (gamma variant stores the same bits)
    case 2:
    case 10:
      return 1;  // 2_10_10_10
    case 3:
    case 12:
      return 2;  // 2_10_10_10_FLOAT (7e3)
    default:
      return -1;
  }
}
}  // namespace

namespace {
// {pixel shader hash, PS constant register of g_mProjectionToWorld}
struct ProjectionReg {
  uint64_t hash;
  uint32_t reg;
};
constexpr ProjectionReg kProjectionRegs[] = {
#include "projection_regs.inc"
};

// Inverse of a 4x4 matrix (row-major, 16 floats); false if singular.
bool Invert4x4(const float m[16], float out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
    }
    if (std::abs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c) std::swap(a[pivot], a[c]);
    double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = float(a[r][c + 4]);
  }
  return true;
}
}  // namespace

void Renderer::CaptureCamera() {
  // The current pixel shader declares g_mProjectionToWorld (catalog
  // reflection): its 4 rows are this frame's inverse view-projection.
  const ProjectionReg* end = std::end(kProjectionRegs);
  const ProjectionReg* it = std::lower_bound(
      std::begin(kProjectionRegs), end, current_ps_hash_,
      [](const ProjectionReg& e, uint64_t h) { return e.hash < h; });
  if (it == end || it->hash != current_ps_hash_ || it->reg + 4 > 256) return;
  const float* c = reinterpret_cast<const float*>(mirror_.regs() + Pm4Mirror::kAluConstantBase +
                                                  1024 + it->reg * 4);
  for (int i = 0; i < 16; ++i) {
    if (!std::isfinite(c[i])) return;
  }
  std::memcpy(inv_view_proj_, c, sizeof(inv_view_proj_));
  camera_valid_ = true;
}

void Renderer::ApplySsao() {
  HostSurface& rt = *scene_rt_;
  HostSurface& ds = *scene_ds_;
  if (rt.depth || !ds.depth || rt.scale != ds.scale || rt.samples != ds.samples) return;
  float view_proj[16];
  if (!Invert4x4(inv_view_proj_, view_proj)) return;
  const uint32_t w = HostPx(rt.width, rt.scale), h = HostPx(rt.height, rt.scale);
  const bool ms = ds.samples > 1;
  if (!ssao_root_signature_) {
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[i].NumDescriptors = 1;
      ranges[i].BaseShaderRegister = i;
    }
    D3D12_ROOT_PARAMETER params[3] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params[i].DescriptorTable.NumDescriptorRanges = 1;
      params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
      params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.Num32BitValues = 40;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 3;
    desc.pParameters = params;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&ssao_root_signature_)))) {
      ssao_ = false;
      return;
    }
  }
  auto pipeline = [&](bool apply) -> ID3D12PipelineState* {
    uint64_t key = (uint64_t(REXCVAR_GET(ssao_debug)) << 41) | (uint64_t(apply) << 40) |
                   (uint64_t(ms) << 39) | (uint64_t(rt.samples) << 32) |
                   uint32_t(rt.view_format);
    auto& pso = ssao_pipelines_[key];
    if (!pso) {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
      desc.pRootSignature = ssao_root_signature_.Get();
      desc.VS = {kBlitVS, sizeof(kBlitVS)};
      if (apply) {
        desc.PS = ms ? D3D12_SHADER_BYTECODE{kSsaoApplyMsPS, sizeof(kSsaoApplyMsPS)}
                     : D3D12_SHADER_BYTECODE{kSsaoApplyPS, sizeof(kSsaoApplyPS)};
        auto& b = desc.BlendState.RenderTarget[0];
        b.BlendEnable = !REXCVAR_GET(ssao_debug);  // debug: show the AO term itself
        b.SrcBlend = D3D12_BLEND_ZERO;  // scene *= ao
        b.DestBlend = D3D12_BLEND_SRC_COLOR;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                  D3D12_COLOR_WRITE_ENABLE_BLUE;
        desc.RTVFormats[0] = rt.view_format;
        desc.SampleDesc.Count = rt.samples;
      } else {
        desc.PS = ms ? D3D12_SHADER_BYTECODE{kSsaoMsPS, sizeof(kSsaoMsPS)}
                     : D3D12_SHADER_BYTECODE{kSsaoPS, sizeof(kSsaoPS)};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
      }
      desc.SampleMask = UINT_MAX;
      desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
      desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
      desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
      desc.NumRenderTargets = 1;
      if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) pso.Reset();
    }
    return pso.Get();
  };
  ID3D12PipelineState* ao_pso = pipeline(false);
  ID3D12PipelineState* apply_pso = pipeline(true);
  if (!ao_pso || !apply_pso) return;
  // AO target (R8, one value per pixel).
  if (!ssao_ao_ || ssao_ao_->GetDesc().Width != w || ssao_ao_->GetDesc().Height != h) {
    Retire(ssao_ao_);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ssao_ao_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, ssao_ao_state_,
                                                nullptr, IID_PPV_ARGS(&ssao_ao_)))) {
      ssao_ao_.Reset();
      return;
    }
    if (!ssao_ao_rtv_.ptr) {
      ssao_ao_rtv_ = provider_->OffsetRTVDescriptor(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                    rtv_heap_next_++ % kRtvHeapSize);
    }
    device_->CreateRenderTargetView(ssao_ao_.Get(), nullptr, ssao_ao_rtv_);
    if (!ssao_ao_srv_) ssao_ao_srv_ = AllocSrvIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        ssao_ao_.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                        ssao_ao_srv_));
  }
  // Depth SRV (the same view the depth-resolve copies use).
  if (!ds.srv_index) {
    ds.srv_index = AllocSrvIndex();
    if (!ds.srv_index) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension = ms ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        ds.resource.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                        ds.srv_index));
  }
  struct {
    float inv_view_proj[16];
    float view_proj[16];
    float size[2];
    float radius, intensity, fade, pad[3];
  } constants{};
  std::memcpy(constants.inv_view_proj, inv_view_proj_, sizeof(inv_view_proj_));
  std::memcpy(constants.view_proj, view_proj, sizeof(view_proj));
  constants.size[0] = float(w);
  constants.size[1] = float(h);
  constants.radius = float(REXCVAR_GET(ssao_radius));
  constants.intensity = float(std::clamp(REXCVAR_GET(ssao_intensity), 0.0, 1.0));
  constants.fade = float(REXCVAR_GET(ssao_fade_distance));
  static_assert(sizeof(constants) == 40 * 4);

  auto gpu_srv = [&](uint32_t index) {
    return provider_->OffsetViewDescriptor(srv_heap_->GetGPUDescriptorHandleForHeapStart(), index);
  };
  Transition(ds.resource.Get(), ds.state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(ssao_ao_.Get(), ssao_ao_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
  command_list_->SetDescriptorHeaps(2, heaps);
  command_list_->SetGraphicsRootSignature(ssao_root_signature_.Get());
  command_list_->SetGraphicsRootDescriptorTable(0, gpu_srv(ds.srv_index));
  command_list_->SetGraphicsRootDescriptorTable(1, gpu_srv(ssao_ao_srv_));
  command_list_->SetGraphicsRoot32BitConstants(2, 40, &constants, 0);
  D3D12_VIEWPORT vp{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
  command_list_->RSSetViewports(1, &vp);
  command_list_->RSSetScissorRects(1, &sc);
  command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  // 1) AO term. The AO SRV (table 1) is bound but unused here: bind a null
  // view for the target being written.
  command_list_->SetGraphicsRootDescriptorTable(1, gpu_srv(0));
  command_list_->SetPipelineState(ao_pso);
  command_list_->OMSetRenderTargets(1, &ssao_ao_rtv_, FALSE, nullptr);
  command_list_->DrawInstanced(3, 1, 0, 0);
  // 2) Blur + multiply into the HDR scene.
  Transition(ssao_ao_.Get(), ssao_ao_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Transition(rt.resource.Get(), rt.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  command_list_->SetGraphicsRootDescriptorTable(1, gpu_srv(ssao_ao_srv_));
  command_list_->SetPipelineState(apply_pso);
  command_list_->OMSetRenderTargets(1, &rt.view, FALSE, nullptr);
  command_list_->DrawInstanced(3, 1, 0, 0);
  Breadcrumb("ssao");
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
}

void Renderer::ApplyFxaa(HostSurface& target) {
  if (!target.resource || target.depth) return;
  if (!fxaa_root_signature_) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 2;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&fxaa_root_signature_)))) {
      fxaa_ = false;
      return;
    }
  }
  auto& pso = fxaa_pipelines_[uint32_t(target.view_format)];
  if (!pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = fxaa_root_signature_.Get();
    desc.VS = {kBlitVS, sizeof(kBlitVS)};
    desc.PS = {kFxaaPS, sizeof(kFxaaPS)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target.view_format;
    desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
      pso.Reset();
      return;
    }
  }
  // Source copy (the pass reads its own target).
  D3D12_RESOURCE_DESC rd = target.resource->GetDesc();
  if (!fxaa_temp_ || fxaa_temp_->GetDesc().Width != rd.Width ||
      fxaa_temp_->GetDesc().Height != rd.Height || fxaa_temp_->GetDesc().Format != rd.Format) {
    Retire(fxaa_temp_);
    D3D12_RESOURCE_DESC td = rd;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    fxaa_temp_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td, fxaa_temp_state_,
                                                nullptr, IID_PPV_ARGS(&fxaa_temp_)))) {
      fxaa_temp_.Reset();
      return;
    }
    if (!fxaa_srv_) fxaa_srv_ = AllocSrvIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = target.srv_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        fxaa_temp_.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), fxaa_srv_));
  }
  Transition(target.resource.Get(), target.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Transition(fxaa_temp_.Get(), fxaa_temp_state_, D3D12_RESOURCE_STATE_COPY_DEST);
  command_list_->CopyResource(fxaa_temp_.Get(), target.resource.Get());
  Transition(fxaa_temp_.Get(), fxaa_temp_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  Transition(target.resource.Get(), target.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
  command_list_->SetDescriptorHeaps(2, heaps);
  command_list_->SetGraphicsRootSignature(fxaa_root_signature_.Get());
  command_list_->SetGraphicsRootDescriptorTable(
      0, provider_->OffsetViewDescriptor(srv_heap_->GetGPUDescriptorHandleForHeapStart(), fxaa_srv_));
  float rcp[2] = {1.0f / float(rd.Width), 1.0f / float(rd.Height)};
  command_list_->SetGraphicsRoot32BitConstants(1, 2, rcp, 0);
  command_list_->SetPipelineState(pso.Get());
  command_list_->OMSetRenderTargets(1, &target.view, FALSE, nullptr);
  D3D12_VIEWPORT vp{0.0f, 0.0f, float(rd.Width), float(rd.Height), 0.0f, 1.0f};
  D3D12_RECT sc{0, 0, LONG(rd.Width), LONG(rd.Height)};
  command_list_->RSSetViewports(1, &vp);
  command_list_->RSSetScissorRects(1, &sc);
  command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list_->DrawInstanced(3, 1, 0, 0);
  // The guest state (root signature, heaps, targets) must be rebound.
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
}

bool Renderer::ReinterpretSurface(HostSurface& dst, HostSurface& src) {
  int src_kind = EdramAliasKind(src.guest_format), dst_kind = EdramAliasKind(dst.guest_format);
  if (src_kind < 0 || dst_kind < 0 || src.width != dst.width || src.height != dst.height ||
      src.samples != dst.samples || src.scale != dst.scale) {
    return false;
  }
  auto& pso = alias_pipelines_[uint32_t(dst.view_format) | (dst.samples << 16)];
  if (!pso) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = alias_root_signature_.Get();
    desc.VS = {kEdramAliasVS, sizeof(kEdramAliasVS)};
    if (dst.samples > 1) {
      desc.PS = {kEdramAliasMsPS, sizeof(kEdramAliasMsPS)};
    } else {
      desc.PS = {kEdramAliasPS, sizeof(kEdramAliasPS)};
    }
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = dst.view_format;
    desc.SampleDesc.Count = dst.samples;
    if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)))) {
      pso.Reset();
      return false;
    }
  }
  if (!src.srv_index) {
    src.srv_index = AllocSrvIndex();
    if (!src.srv_index) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = src.srv_format;
    srv.ViewDimension =
        src.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (src.samples <= 1) srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        src.resource.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                        src.srv_index));
  }
  Transition(src.resource.Get(), src.state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
  command_list_->SetDescriptorHeaps(2, heaps);
  command_list_->SetGraphicsRootSignature(alias_root_signature_.Get());
  uint32_t kinds[2] = {uint32_t(src_kind), uint32_t(dst_kind)};
  command_list_->SetGraphicsRoot32BitConstants(0, 2, kinds, 0);
  command_list_->SetGraphicsRootDescriptorTable(
      1, provider_->OffsetViewDescriptor(srv_heap_->GetGPUDescriptorHandleForHeapStart(),
                                         src.srv_index));
  command_list_->SetPipelineState(pso.Get());
  command_list_->OMSetRenderTargets(1, &dst.view, FALSE, nullptr);
  D3D12_VIEWPORT vp{0.0f, 0.0f, float(HostPx(dst.width, dst.scale)),
                    float(HostPx(dst.height, dst.scale)), 0.0f, 1.0f};
  D3D12_RECT sc{0, 0, LONG(HostPx(dst.width, dst.scale)), LONG(HostPx(dst.height, dst.scale))};
  command_list_->RSSetViewports(1, &vp);
  command_list_->RSSetScissorRects(1, &sc);
  command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list_->DrawInstanced(3, 1, 0, 0);
  Breadcrumb("edram reinterpret");
  // Guest state must be rebound (root signature, heaps, targets).
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
  return true;
}

void Renderer::ResolveEdramAliasing(HostSurface* s) {
  if (!s || !s->edram_tiles || !REXCVAR_GET(native_edram_aliasing)) return;
  constexpr uint32_t kEdramTiles = 2048;
  auto overlaps = [](uint32_t a0, uint32_t an, uint32_t b0, uint32_t bn) {
    // Ranges on the 2048-tile ring.
    for (uint32_t shift : {0u, kEdramTiles}) {
      if (a0 + shift < b0 + bn && b0 < a0 + shift + an) return true;
      if (b0 + shift < a0 + an && a0 < b0 + shift + bn) return true;
    }
    return false;
  };
  uint64_t newest = 0;
  HostSurface* newest_same = nullptr;
  for (auto& [key, other] : surfaces_) {
    if (&other == s || other.last_write <= s->last_write) continue;
    if (std::find(std::begin(bind_set_), std::end(bind_set_), &other) != std::end(bind_set_)) {
      continue;
    }
    if (other.edram_base == s->edram_base && other.depth == s->depth) {
      // Same EDRAM placement, other format: the bits are shared (e.g. the
      // p9 8888 foliage/decal buffer reads the previous HDR 7e3 scene).
      if (!s->depth && other.width == s->width && other.height == s->height &&
          other.scale == s->scale &&
          (!newest_same || other.last_write > newest_same->last_write)) {
        newest_same = &other;
      }
      continue;
    }
    if (overlaps(s->edram_base, s->edram_tiles, other.edram_base, other.edram_tiles)) {
      newest = std::max(newest, other.last_write);
    }
  }
  if (trace_state_ == 1) {
    REXLOG_INFO("trace edram aliasing {}: newest same {} ({}) newest other {} own {}",
                SurfaceName(s), newest_same ? SurfaceName(newest_same) : "-",
                newest_same ? newest_same->last_write : 0, newest, s->last_write);
  }
  if (newest_same && newest_same->last_write > newest &&
      REXCVAR_GET(native_edram_reinterpret)) {
    bool ok = ReinterpretSurface(*s, *newest_same);
    if (trace_state_ == 1)
      REXLOG_INFO("trace edram reinterpret {} <- {}: {} (kinds {} {} fmt {} {})", SurfaceName(s),
                  SurfaceName(newest_same), ok, newest_same->guest_format, s->guest_format,
                  int(newest_same->srv_format), int(s->view_format));
    if (ok) {
      ++stats_.edram_alias_clears;
      s->last_write = ++edram_write_seq_;
      return;
    }
  }
  if (!newest) return;
  if (s->depth) {
    Transition(s->resource.Get(), s->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_list_->ClearDepthStencilView(s->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                         1.0f, 0, 0, nullptr);
  } else {
    // Undefined EDRAM contents: black. Alpha 1 by default - EDRAM left behind
    // by the previous frame's opaque images reads as alpha ~1 (e.g. the p9
    // decal buffer's alpha gates terrain specular; 0 made the beach white).
    float a = float(REXCVAR_GET(native_edram_alias_alpha));
    float value[4] = {0.0f, 0.0f, 0.0f, a};
    Transition(s->resource.Get(), s->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list_->ClearRenderTargetView(s->view, value, 0, nullptr);
  }
  ++stats_.edram_alias_clears;
  Breadcrumb("edram alias clear");
  s->last_write = ++edram_write_seq_;
}

bool Renderer::BindRenderTargets(uint8_t* base, uint32_t dev, DXGI_FORMAT rtv_formats[4],
                                 uint32_t& rt_count, DXGI_FORMAT& dsv_format) {
  HostSurface* rts[4] = {};
  rt_count = 0;
  // Only RTs the draw can write: stale MRT slots left bound by the guest (with
  // a zero write mask) would clip D3D12 rendering to the smallest bound target.
  uint32_t color_mask = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t surface = Load32(base, dev + kDevRenderTargets + 4 * i);
    if (i && !((color_mask >> (4 * i)) & 0xF)) surface = 0;
    if (surface) {
      rts[i] = GetSurface(base, surface, false);
      if (rts[i]) rt_count = i + 1;
    }
  }
  // D3D12 renders only the intersection of all bound targets; Xenos does not
  // care when the guest pairs a larger RT with a smaller depth buffer (e.g.
  // the 1280x720 upscale target with the 1024x576 scene depth), so use a depth
  // surface covering RT0 in that case.
  HostSurface* ds = GetSurface(base, Load32(base, dev + kDevDepthStencil), true,
                               rts[0] ? rts[0]->width : 0, rts[0] ? rts[0]->height : 0);
  if (!rt_count && !ds) {
    return false;
  }
  bound_samples_ = rts[0] ? rts[0]->samples : ds ? ds->samples : 1;
  bound_scale_ = rts[0] ? rts[0]->scale : ds ? ds->scale : 1.0f;
  for (uint32_t i = 1; i < 4; ++i) {
    if (rts[i] && (rts[i]->samples != bound_samples_ || rts[i]->scale != bound_scale_)) {
      rts[i] = nullptr;
    }
  }
  if (ds && ds->scale != bound_scale_) {
    static int logged = 0;
    if (logged++ < 8) {
      REXLOG_WARN("native: depth surface scale ({}) differs from RT ({}), depth unbound",
                  ds->scale, bound_scale_);
    }
    ds = nullptr;
  }
  if (ds && ds->samples != bound_samples_) {
    static int logged = 0;
    if (logged++ < 8) {
      REXLOG_WARN("native: depth surface MSAA ({}) differs from RT ({}), depth unbound",
                  ds->samples, bound_samples_);
    }
    ds = nullptr;
  }
  // Experiment: EDRAM-aliased depth surfaces (e.g. the 256x144 bloom depth at
  // base 448, never cleared by the game) start from depth 0 each frame.
  if (ds && REXCVAR_GET(native_exp_alias_depth_base) >= 0 &&
      ds->edram_base == uint32_t(REXCVAR_GET(native_exp_alias_depth_base)) &&
      alias_clear_frame_ != frame_count_) {
    alias_clear_frame_ = frame_count_;
    Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_list_->ClearDepthStencilView(ds->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                         float(REXCVAR_GET(native_exp_alias_depth_value)), 0, 0,
                                         nullptr);
  }
  // Aliasing is checked against surfaces outside this bind set; the set is
  // then marked as the latest EDRAM writer.
  bool same_bind = ds == bind_set_[4];
  for (uint32_t i = 0; i < 4 && same_bind; ++i) same_bind = rts[i] == bind_set_[i];
  if (g_current_pass == kPassUpscale && rts[0] && rts[0]->samples == 1) upscale_rt_ = rts[0];
  if (g_current_pass == kPassRenderOpaque && rts[0] && ds) {
    scene_rt_ = rts[0];
    scene_ds_ = ds;
  }
  if (!same_bind) {
    // Only a new bind set can observe other surfaces' EDRAM writes.
    bind_set_[0] = rts[0]; bind_set_[1] = rts[1]; bind_set_[2] = rts[2]; bind_set_[3] = rts[3];
    bind_set_[4] = ds;
    for (uint32_t i = 0; i < 4; ++i) ResolveEdramAliasing(rts[i]);
    ResolveEdramAliasing(ds);
    uint64_t seq = ++edram_write_seq_;
    for (uint32_t i = 0; i < 4; ++i) if (rts[i]) rts[i]->last_write = seq;
    if (ds) ds->last_write = seq;
  }
  rt_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (rts[i]) rt_count = i + 1;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    rtv_formats[i] = rts[i] ? rts[i]->view_format : DXGI_FORMAT_UNKNOWN;
    if (rts[i]) Transition(rts[i]->resource.Get(), rts[i]->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  dsv_format = ds ? ds->view_format : DXGI_FORMAT_UNKNOWN;
  if (ds) Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  bool changed = rt_count != bound_rt_count_ || ds != bound_ds_;
  for (uint32_t i = 0; i < 4 && !changed; ++i) changed = rts[i] != bound_rts_[i];
  if (changed) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4] = {};
    for (uint32_t i = 0; i < rt_count; ++i) {
      // Gaps are not allowed with a contiguous RTV array; use the first bound
      // RT's view for gaps (write mask 0 in that case is the guest's intent).
      rtvs[i] = rts[i] ? rts[i]->view : rts[0] ? rts[0]->view : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    command_list_->OMSetRenderTargets(rt_count, rt_count ? rtvs : nullptr, FALSE,
                                      ds ? &ds->view : nullptr);
    if (trace_state_ == 1) {
      REXLOG_INFO("trace p{} after {} draws: bind RT [{} {} {} {}] DS {}", g_current_pass,
                  trace_draws_, SurfaceName(rts[0]), SurfaceName(rts[1]), SurfaceName(rts[2]),
                  SurfaceName(rts[3]), SurfaceName(ds));
      trace_draws_ = 0;
    }
    std::memcpy(bound_rts_, rts, sizeof(rts));
    bound_rt_count_ = rt_count;
    bound_ds_ = ds;
  }
  return true;
}


// Thread-safe lookup into the embedded shader pack (pipeline precompilation).
bool PackShaderLookup(uint64_t hash, bool vertex, const void** data, size_t* size) {
  const ShaderPack& pack = EmbeddedShaderPack();
  if (!pack.count || !REXCVAR_GET(native_shader_dir).empty()) return false;
  uint32_t stage = vertex ? 0 : 1;
  uint32_t lo = 0, hi = pack.count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    PackEntry m = pack.Entry(mid);
    if (m.hash < hash || (m.hash == hash && m.stage < stage)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo >= pack.count) return false;
  PackEntry e = pack.Entry(lo);
  if (e.hash != hash || e.stage != stage) return false;
  *data = pack.base + e.offset;
  *size = e.size;
  return true;
}

const std::vector<uint8_t>* Renderer::LoadShader(uint64_t hash, bool vertex) {
  auto it = shader_bytecode_.find(hash);
  if (it != shader_bytecode_.end()) {
    return it->second.get();
  }
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX.%s.dxil", (unsigned long long)hash,
                vertex ? "vs" : "ps");
  std::unique_ptr<std::vector<uint8_t>> data;
  // Embedded pack unless native_shader_dir points at loose files.
  const ShaderPack& pack = EmbeddedShaderPack();
  if (pack.count && REXCVAR_GET(native_shader_dir).empty()) {
    uint32_t stage = vertex ? 0 : 1;
    // Binary search over (hash, stage).
    uint32_t lo = 0, hi = pack.count;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      PackEntry m = pack.Entry(mid);
      if (m.hash < hash || (m.hash == hash && m.stage < stage)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    PackEntry e{};
    if (lo < pack.count) e = pack.Entry(lo);
    if (lo < pack.count && e.hash == hash && e.stage == stage) {
      data = std::make_unique<std::vector<uint8_t>>(pack.base + e.offset,
                                                    pack.base + e.offset + e.size);
    } else {
      REXLOG_WARN("native: shader {} not in the embedded pack", name);
    }
    auto* result = data.get();
    shader_bytecode_[hash] = std::move(data);
    return result;
  }
  std::ifstream f(std::filesystem::path(shader_dir_) / name, std::ios::binary);
  if (f) {
    data = std::make_unique<std::vector<uint8_t>>((std::istreambuf_iterator<char>(f)),
                                                  std::istreambuf_iterator<char>());
  } else {
    REXLOG_WARN("native: missing shader {}", name);
  }
  auto* result = data.get();
  shader_bytecode_[hash] = std::move(data);
  return result;
}

ID3D12PipelineState* Renderer::GetPipeline(uint8_t* base, uint32_t dev, uint64_t vs_hash,
                                           uint64_t ps_hash, uint32_t decl,
                                           D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type,
                                           const DXGI_FORMAT rtv_formats[4], uint32_t rt_count,
                                           DXGI_FORMAT dsv_format,
                                           D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut) {
  reg::RB_BLENDCONTROL blend{LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0)};
  reg::PA_SU_SC_MODE_CNTL mode{LoadReg(base, dev, rex::graphics::XE_GPU_REG_PA_SU_SC_MODE_CNTL)};
  reg::RB_DEPTHCONTROL depth{LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL)};
  uint32_t color_mask = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLOR_MASK);
  uint32_t stencil_ref_mask = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
  uint32_t decl_count = decl ? Load32(base, decl + 0x18) : 0;
  uint64_t decl_hash = decl ? XXH3_64bits(GuestPtr(base, decl + 0x34, decl_count * 12), size_t(decl_count) * 12) : 0;
  // Polygon offset (decals over coplanar terrain). PA_SU_POLY_OFFSET_* are not
  // in the XDK register shadow: read them from the PM4 mirror (same choice of
  // face as Xenia's GetPreferredFacePolygonOffset).
  float poly_scale = 0.0f, poly_offset = 0.0f;
  {
    auto regf = [&](uint32_t r) {
      uint32_t v = mirror_.reg(r);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    };
    bool polygonal = topology_type == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (polygonal) {
      if (mode.poly_offset_front_enable && !mode.cull_front) {
        poly_scale = regf(rex::graphics::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset = regf(rex::graphics::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (mode.poly_offset_back_enable && !mode.cull_back && !poly_scale && !poly_offset) {
        poly_scale = regf(rex::graphics::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset = regf(rex::graphics::XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else if (mode.poly_offset_para_enable) {
      poly_scale = regf(rex::graphics::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
      poly_offset = regf(rex::graphics::XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
    }
    if (!std::isfinite(poly_scale)) poly_scale = 0.0f;
    if (!std::isfinite(poly_offset)) poly_offset = 0.0f;
  }
  int32_t depth_bias = rex::graphics::draw_util::GetD3D10IntegerPolygonOffset(
      rex::graphics::xenos::DepthRenderTargetFormat::kD24S8, poly_offset);
  float slope_bias = poly_scale * rex::graphics::xenos::kPolygonOffsetScaleSubpixelUnit;
  if (trace_state_ == 1) {
    REXLOG_INFO("trace   pso mode {:08X} poly scale {} offset {} bias {} depthctl {:08X} refmask {:08X} "
                "samples {} a2c {}",
                mode.value, poly_scale, poly_offset, depth_bias, depth.value, stencil_ref_mask,
                bound_samples_, (LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLORCONTROL) >> 4) & 1);
  }
  uint32_t slope_bits;
  std::memcpy(&slope_bits, &slope_bias, 4);

  // Alpha to mask (RB_COLORCONTROL bit 4): MSAA coverage from alpha (foliage).
  bool alpha_to_coverage =
      (bound_samples_ > 1 && REXCVAR_GET(native_alpha_to_coverage) &&
       ((LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLORCONTROL) >> 4) & 1)) ||
      AlphaTestToCoverage(base, dev);
  uint64_t key_parts[] = {vs_hash ^ (uint64_t(bound_samples_) << 60) ^
                              (uint64_t(alpha_to_coverage) << 59),
                          (uint64_t(uint32_t(depth_bias)) << 32) | slope_bits,
                          ps_hash,
                          decl_hash,
                          blend.value,
                          uint64_t(mode.value & 0x7) | (uint64_t(color_mask & 0xFFFF) << 8) |
                              (uint64_t(topology_type) << 40),
                          depth.value | (uint64_t(stencil_ref_mask & 0xFFFF00) << 32),
                          uint64_t(rtv_formats[0]) | (uint64_t(rtv_formats[1]) << 8) |
                              (uint64_t(rtv_formats[2]) << 16) | (uint64_t(rtv_formats[3]) << 24) |
                              (uint64_t(rt_count) << 32) | (uint64_t(dsv_format) << 40) |
                              (uint64_t(strip_cut) << 56)};
  uint64_t key = XXH3_64bits(key_parts, sizeof(key_parts));
  if (!async_pipelines_.empty()) {
    auto ap = async_pipelines_.find(key);
    if (ap != async_pipelines_.end()) {
      ComPtr<ID3D12PipelineState> ready;
      switch (pipeline_cache_.TryTake(key, ready)) {
        case PipelineCache::Poll::kReady:
          async_pipelines_.erase(ap);
          pipelines_[key] = ready;
          return ready.Get();
        case PipelineCache::Poll::kFailed:
          async_pipelines_.erase(ap);
          pipelines_[key] = nullptr;
          return nullptr;
        default:
          ++stats_.pso_async_skipped_draws;
          return nullptr;
      }
    }
  }
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second.Get();
  }
  pipelines_[key] = nullptr;
  {
    // Precompiled at startup (or compiling right now) by the pipeline cache.
    ComPtr<ID3D12PipelineState> precompiled;
    if (pipeline_cache_.Take(key, precompiled)) {
      pipelines_[key] = precompiled;
      ++stats_.pso_precompiled_hits;
      return precompiled.Get();
    }
  }

  const auto* vs = LoadShader(vs_hash, true);
  // ps_hash == 0: depth-only draw (the game binds no pixel shader).
  const auto* ps = ps_hash ? LoadShader(ps_hash, false) : nullptr;
  if (!vs || (ps_hash && !ps)) {
    return nullptr;
  }

  std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
  PsoRecord record;
  for (uint32_t i = 0; i < decl_count; ++i) {
    uint32_t e = decl + 0x34 + 12 * i;
    uint32_t stream = Load16(base, e);
    uint32_t offset = Load16(base, e + 2);
    uint32_t type = Load32(base, e + 4);
    uint32_t usage = Load8(base, e + 9);
    uint32_t usage_index = Load8(base, e + 10);
    if (stream == 0xFF) break;
    DeclFormat fmt = MapDeclType(type);
    if (fmt.format == DXGI_FORMAT_UNKNOWN) continue;
    D3D12_INPUT_ELEMENT_DESC d{};
    d.SemanticName = UsageSemantic(usage);
    d.SemanticIndex = usage_index;
    d.Format = fmt.format;
    d.InputSlot = stream;
    d.AlignedByteOffset = offset;
    d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    elements.push_back(d);
    if (record.element_count < PsoRecord::kMaxElements) {
      auto& re = record.elements[record.element_count++];
      re.usage = uint8_t(usage);
      re.usage_index = uint8_t(usage_index);
      re.slot = uint8_t(stream);
      re.format = uint32_t(fmt.format);
      re.offset = offset;
    }
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root_signature_.Get();
  desc.VS = {vs->data(), vs->size()};
  if (ps) {
    desc.PS = {ps->data(), ps->size()};
  }
  desc.InputLayout = {elements.data(), uint32_t(elements.size())};
  D3D12_RENDER_TARGET_BLEND_DESC rt{};
  rt.SrcBlend = MapBlend(blend.color_srcblend);
  rt.DestBlend = MapBlend(blend.color_destblend);
  rt.BlendOp = MapBlendOp(blend.color_comb_fcn);
  rt.SrcBlendAlpha = MapBlendAlpha(blend.alpha_srcblend);
  rt.DestBlendAlpha = MapBlendAlpha(blend.alpha_destblend);
  rt.BlendOpAlpha = MapBlendOp(blend.alpha_comb_fcn);
  rt.BlendEnable = !(rt.SrcBlend == D3D12_BLEND_ONE && rt.DestBlend == D3D12_BLEND_ZERO &&
                     rt.BlendOp == D3D12_BLEND_OP_ADD && rt.SrcBlendAlpha == D3D12_BLEND_ONE &&
                     rt.DestBlendAlpha == D3D12_BLEND_ZERO &&
                     rt.BlendOpAlpha == D3D12_BLEND_OP_ADD);
  rt.LogicOp = D3D12_LOGIC_OP_NOOP;
  desc.BlendState.IndependentBlendEnable = TRUE;
  desc.BlendState.AlphaToCoverageEnable = alpha_to_coverage;
  for (uint32_t i = 0; i < 4; ++i) {
    desc.BlendState.RenderTarget[i] = rt;
    desc.BlendState.RenderTarget[i].RenderTargetWriteMask = uint8_t((color_mask >> (4 * i)) & 0xF);
    // Integer/float formats that can't blend.
    if (rtv_formats[i] == DXGI_FORMAT_R32_FLOAT || rtv_formats[i] == DXGI_FORMAT_R32G32_FLOAT) {
      desc.BlendState.RenderTarget[i].BlendEnable = FALSE;
    }
  }
  desc.SampleMask = UINT_MAX;
  desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  if (mode.cull_front && !mode.cull_back) {
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
  } else if (mode.cull_back && !mode.cull_front) {
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
  } else {
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  }
  desc.RasterizerState.FrontCounterClockwise =
      (mode.face ? FALSE : TRUE) ^ (REXCVAR_GET(native_flip_winding) ? TRUE : FALSE);
  desc.RasterizerState.DepthClipEnable = TRUE;
  desc.RasterizerState.DepthBias = depth_bias;
  desc.RasterizerState.SlopeScaledDepthBias = slope_bias;
  if (dsv_format != DXGI_FORMAT_UNKNOWN) {
    auto& ds = desc.DepthStencilState;
    ds.DepthEnable = depth.z_enable && !REXCVAR_GET(native_debug_no_ztest);
    ds.DepthWriteMask = depth.z_write_enable ? D3D12_DEPTH_WRITE_MASK_ALL
                                             : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = MapCompare(depth.zfunc);
    ds.StencilEnable = depth.stencil_enable;
    ds.StencilReadMask = uint8_t(stencil_ref_mask >> 8);
    ds.StencilWriteMask = uint8_t(stencil_ref_mask >> 16);
    ds.FrontFace.StencilFunc = MapCompare(depth.stencilfunc);
    ds.FrontFace.StencilFailOp = MapStencilOp(depth.stencilfail);
    ds.FrontFace.StencilPassOp = MapStencilOp(depth.stencilzpass);
    ds.FrontFace.StencilDepthFailOp = MapStencilOp(depth.stencilzfail);
    if (depth.backface_enable) {
      ds.BackFace.StencilFunc = MapCompare(depth.stencilfunc_bf);
      ds.BackFace.StencilFailOp = MapStencilOp(depth.stencilfail_bf);
      ds.BackFace.StencilPassOp = MapStencilOp(depth.stencilzpass_bf);
      ds.BackFace.StencilDepthFailOp = MapStencilOp(depth.stencilzfail_bf);
    } else {
      ds.BackFace = ds.FrontFace;
    }
    desc.DSVFormat = dsv_format;
  }
  desc.PrimitiveTopologyType = topology_type;
  desc.IBStripCutValue = strip_cut;
  desc.NumRenderTargets = rt_count;
  for (uint32_t i = 0; i < rt_count; ++i) {
    desc.RTVFormats[i] = rtv_formats[i] != DXGI_FORMAT_UNKNOWN ? rtv_formats[i] : rtv_formats[0];
  }
  desc.SampleDesc.Count = bound_samples_;
  auto fill_record = [&] {
    record.key = key;
    record.vs_hash = vs_hash;
    record.ps_hash = ps_hash;
    record.blend = desc.BlendState;
    record.raster = desc.RasterizerState;
    record.depth_stencil = desc.DepthStencilState;
    for (uint32_t i = 0; i < 8; ++i) record.rtv_formats[i] = desc.RTVFormats[i];
    record.dsv_format = desc.DSVFormat;
    record.num_render_targets = desc.NumRenderTargets;
    record.sample_count = desc.SampleDesc.Count;
    record.topology_type = uint32_t(desc.PrimitiveTopologyType);
    record.strip_cut = uint32_t(desc.IBStripCutValue);
  };
  if (elements.size() <= PsoRecord::kMaxElements) {
    // Same description precompiled under another key: a settings variant
    // generated from a pipeline captured with other MSAA/foliage settings, or
    // guest state bits that do not change the host pipeline (EXP-052).
    fill_record();
    ComPtr<ID3D12PipelineState> variant;
    if (pipeline_cache_.TakeByDesc(record, variant)) {
      pipeline_cache_.Record(record);
      ++stats_.pso_precompiled_hits;
      pipelines_[key] = variant;
      return variant.Get();
    }
  }
  // Asynchronous compilation (EXP-053) only in the passes the game redraws from
  // scratch every frame: a skipped draw there reappears as soon as its
  // pipeline is ready. Everything that may carry state across frames (post
  // process, the luminance/exposure chain, copies, HUD, menus) still waits: a
  // skipped one-time draw can break the image permanently (EXP-005).
  if (REXCVAR_GET(native_async_pipelines) && REXCVAR_GET(native_pipeline_cache) &&
      elements.size() <= PsoRecord::kMaxElements) {
    int p = g_current_pass;
    bool redrawn = p == kPassRenderShadowMaps || p == 6 || p == 8 || p == 9 || p == kPassRenderOpaque ||
                   p == 13 || p == 14 || p == 15 || p == 17 || p == kPassRenderSorted;
    if (redrawn) {
      pipeline_cache_.Record(record);
      pipeline_cache_.CompileAsync(record);
      pipelines_.erase(key);
      async_pipelines_.insert(key);
      ++stats_.pso_async_requests;
      ++stats_.pso_async_skipped_draws;
      REXLOG_INFO("native: PSO compiling asynchronously (pass {} vs {:016X} ps {:016X})", p,
                  vs_hash, ps_hash);
      return nullptr;
    }
  }
  ComPtr<ID3D12PipelineState> pso;
  auto create_start = std::chrono::steady_clock::now();
  HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) {
    REXLOG_ERROR("native: PSO creation failed ({:08X}) vs={:016X} ps={:016X} elements={} rts={}",
                 uint32_t(hr), vs_hash, ps_hash, elements.size(), rt_count);
    return nullptr;
  }
  {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          create_start).count();
    stats_.pso_create_ms += ms;
    REXLOG_INFO("native: PSO #{} created in {:.2f} ms (pass {} vs {:016X} ps {:016X})",
                stats_.pso_created + 1, ms, g_current_pass, vs_hash, ps_hash);
  }
  // Remember it for the next start's precompilation.
  if (elements.size() <= PsoRecord::kMaxElements) {
    pipeline_cache_.Record(record);
  }
  ++stats_.pso_created;
  pipelines_[key] = pso;
  return pso.Get();
}

uint32_t Renderer::GetTextureSrvIndex(uint8_t* base, const uint32_t fetch[6]) {
  // Textures produced by resolves live on the host GPU only.
  uint32_t base_address = GuestPhysical(fetch[1] & 0xFFFFF000u);
  auto latest = resolve_latest_.find(base_address);
  auto rit = latest != resolve_latest_.end() ? resolve_textures_.find(latest->second)
                                             : resolve_textures_.end();
  if (rit != resolve_textures_.end()) {
    ResolveTexture& rt = rit->second;
    xenos::xe_gpu_texture_fetch_t f;
    std::memcpy(&f, fetch, sizeof(f));
    // Component mapping from the fetch swizzle (source components clamped to
    // the resolve format's channel count happen implicitly - missing
    // channels read 0/1 in D3D12).
    uint32_t mapping[4];
    static const uint32_t kSwapRB[4] = {2, 1, 0, 3};
    for (int i = 0; i < 4; ++i) {
      uint32_t s = (f.swizzle >> (3 * i)) & 7;
      if (s < 4 && rt.swap_rb) s = kSwapRB[s];
      mapping[i] = s < 4 ? s
                         : (s == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0
                                   : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
      if (rt.srv_format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
          rt.srv_format == DXGI_FORMAT_R32_FLOAT) {
        if (mapping[i] < 4) mapping[i] = 0;  // single-channel: replicate
      }
    }
    uint32_t encoded = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2],
                                                               mapping[3]);
    // A depth resolve fetched as k_8_8_8_8 reads the raw D24S8 bytes.
    if (rt.raw_resource && (fetch[1] & 0x3F) == uint32_t(xenos::TextureFormat::k_8_8_8_8)) {
      uint32_t raw_mapping[4];
      for (int i = 0; i < 4; ++i) {
        uint32_t s = (f.swizzle >> (3 * i)) & 7;
        raw_mapping[i] = s < 4 ? s
                               : (s == 4 ? D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0
                                         : D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1);
      }
      uint32_t raw_encoded = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
          raw_mapping[0], raw_mapping[1], raw_mapping[2], raw_mapping[3]);
      auto rit2 = rt.raw_srv_by_mapping.find(raw_encoded);
      if (rit2 != rt.raw_srv_by_mapping.end()) return rit2->second;
      uint32_t index = AllocSrvIndex();
      D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
      srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = raw_encoded;
      srv.Texture2D.MipLevels = 1;
      device_->CreateShaderResourceView(
          rt.raw_resource.Get(), &srv,
          provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), index));
      rt.raw_srv_by_mapping[raw_encoded] = index;
      return index;
    }
    auto sit = rt.srv_by_mapping.find(encoded);
    if (sit != rt.srv_by_mapping.end()) {
      return sit->second;
    }
    uint32_t index = AllocSrvIndex();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = rt.srv_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = encoded;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        rt.resource.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(), index));
    rt.srv_by_mapping[encoded] = index;
    return index;
  }

  // Sign fields (dword 0 bits 2-9) are part of the key: gamma changes the view.
  uint64_t key_parts[4] = {fetch[1] | (uint64_t((fetch[0] >> 2) & 0xFF) << 32), fetch[2],
                           fetch[3] & 0x1FFE, fetch[5] & 0xFFFFFE00u};
  uint64_t key = XXH3_64bits(key_parts, sizeof(key_parts));
  auto it = textures_.find(key);
  if (it != textures_.end()) {
    TextureEntry& entry = it->second;
    if (entry.checked_frame != frame_count_ && entry.guest_size) {
      entry.checked_frame = frame_count_;
      // With write watches: only after a guest write to its pages. Otherwise
      // small textures are revalidated every frame, large ones every 30.
      bool check = texture_watch_
                       ? TextureWrittenSince(entry.guest_base, entry.guest_size, entry.watch_seq)
                       : entry.guest_size <= (256u << 10) || frame_count_ - entry.hashed_frame >= 30;
      if (REXCVAR_GET(native_debug_texture_format) >= 0 &&
          int32_t(fetch[1] & 0x3F) == REXCVAR_GET(native_debug_texture_format)) {
        static int logged = 0;
        uint64_t h = XXH3_64bits(
            REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(entry.guest_base), entry.guest_size);
        if (logged < 400 && (check || h != entry.content_hash)) {
          ++logged;
          REXLOG_INFO("native dbg tex check {:08X} frame {} written {} hash {} now {} seq {} global {}",
                      entry.guest_base, frame_count_, check, entry.content_hash, h, entry.watch_seq,
                      write_seq_.load());
        }
      }
      if (check) {
        entry.hashed_frame = frame_count_;
        if (texture_watch_) {
          entry.watch_seq = ArmTextureWatch(entry.guest_base, entry.guest_size);
        }
        const uint8_t* src =
            REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(entry.guest_base);
        uint64_t hash = XXH3_64bits(src, entry.guest_size);
        if (hash != entry.content_hash) {
          ++stats_.textures_reloaded;
          Retire(entry.resource);
          RetireSrvIndex(entry.srv_index);
          entry.resource.Reset();
          entry.srv_index = 0;
          if (!CreateTexture(fetch, entry)) {
            textures_.erase(it);
            return 0;
          }
        }
      }
    }
    return entry.srv_index;
  }
  TextureEntry& entry = textures_[key];
  if (!CreateTexture(fetch, entry)) {
    textures_.erase(key);
    return 0;
  }
  return entry.srv_index;
}

void Renderer::RetireSrvIndex(uint32_t index) {
  if (index > 2) retired_srvs_.emplace_back(next_fence_value_, index);
}

std::pair<uint32_t, uint32_t> Renderer::OnPhysicalWrite(void* context, uint32_t start,
                                                        uint32_t length, bool exact_range) {
  auto* self = static_cast<Renderer*>(context);
  uint32_t seq = self->write_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
  uint32_t first = start >> 12;
  uint32_t last = std::min((uint64_t(start) + std::max(length, 1u) - 1) >> 12, uint64_t(0x1FFFF));
  for (uint32_t p = first; p <= last; ++p) {
    self->page_write_seq_[p].store(seq, std::memory_order_release);
  }
  // Only the written pages are unwatched (and marked above).
  return {start, length};
}

uint32_t Renderer::ArmTextureWatch(uint32_t base, uint32_t size) {
  // Sequence taken before protecting: any later write is newer.
  uint32_t seq = write_seq_.load(std::memory_order_acquire);
  REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(base & 0x1FFFFFFF, size, true, false);
  return seq;
}

bool Renderer::TextureWrittenSince(uint32_t base, uint32_t size, uint32_t seq) const {
  uint32_t first = (base & 0x1FFFFFFF) >> 12;
  uint32_t last = std::min(((base & 0x1FFFFFFF) + size - 1) >> 12, 0x1FFFFu);
  for (uint32_t p = first; p <= last; ++p) {
    if (int32_t(page_write_seq_[p].load(std::memory_order_acquire) - seq) > 0) return true;
  }
  return false;
}

uint32_t Renderer::CreateTexture(const uint32_t fetch[6], TextureEntry& entry) {
  // Watch before reading: arm, hash, then decode. A guest write after arming
  // (e.g. a streaming loader filling the texture while the worker decodes it)
  // is always seen by the next revalidation.
  uint32_t pre_base = 0, pre_size = 0, pre_seq = 0;
  uint64_t pre_hash = 0;
  bool pre_armed = texture_watch_ && GetTextureBaseRange(fetch, pre_base, pre_size);
  if (pre_armed) {
    pre_seq = ArmTextureWatch(pre_base, pre_size);
    pre_hash = XXH3_64bits(REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(pre_base),
                           pre_size);
  }
  DecodedTexture decoded;
  const char* reason = nullptr;
  bool decoded_ok = DecodeTexture(fetch, decoded, &reason);
  if (decoded_ok && REXCVAR_GET(native_debug_texture_format) >= 0 &&
      int32_t(decoded.guest_format) == REXCVAR_GET(native_debug_texture_format)) {
    REXLOG_INFO("native dbg texture fmt {} base {:08X} {}x{} dxgi {} mips {} fetch {:08X} {:08X} {:08X} {:08X}",
                decoded.guest_format, decoded.base_address, decoded.width, decoded.height,
                int(decoded.format), decoded.mip_levels, fetch[0], fetch[1], fetch[2], fetch[3]);
  }
  bool dump_by_format = REXCVAR_GET(native_debug_texture_format) >= 0 &&
                        int32_t(decoded.guest_format) == REXCVAR_GET(native_debug_texture_format) &&
                        decoded.width <= 512 && !REXCVAR_GET(native_dump_dir).empty();
  if (decoded_ok && (dump_by_format || (!REXCVAR_GET(native_dump_texture_addr).empty() &&
      REXCVAR_GET(native_dump_texture_addr).find(fmt::format("{:08X}", decoded.base_address)) !=
          std::string::npos))) {
    std::string path = REXCVAR_GET(native_dump_dir) + fmt::format("/tex_{:08X}_{}.dds",
                                                                  decoded.base_address, frame_count_);
    WriteDds(decoded, path.c_str());
    REXLOG_INFO("native: dumped texture {:08X} fmt {} endian {} -> {}", decoded.base_address,
                decoded.guest_format, (fetch[1] >> 6) & 3, path);
  }
  bool cube = decoded.dimension == uint32_t(xenos::DataDimension::kCube);
  bool volume = decoded.dimension == uint32_t(xenos::DataDimension::k3D);
  if (decoded_ok && !cube && !volume && decoded.depth != 1) {
    decoded_ok = false;
    reason = "stacked 2D array";
  }
  if (!decoded_ok) {
    static std::set<uint64_t> logged;
    uint64_t k = (uint64_t(fetch[1]) << 32) | fetch[5];
    if (logged.size() < 200 && logged.insert(k).second) {
      REXLOG_INFO("native: texture decode failed ({}): pass {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} dim {} fmt {}",
                  reason ? reason : "?", g_current_pass, fetch[0], fetch[1], fetch[2], fetch[3],
                  fetch[4], fetch[5], (fetch[5] >> 9) & 3, fetch[1] & 0x3F);
    }
    return 0;
  }
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = volume ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  bool block_compressed = decoded.format >= DXGI_FORMAT_BC1_TYPELESS &&
                          decoded.format <= DXGI_FORMAT_BC5_SNORM;
  desc.Width = block_compressed ? (decoded.width + 3) & ~3u : decoded.width;
  desc.Height = block_compressed ? (decoded.height + 3) & ~3u : decoded.height;
  desc.DepthOrArraySize = uint16_t(decoded.depth);
  desc.MipLevels = uint16_t(decoded.mip_levels);
  // Xenos gamma textures (sign = GAMMA on RGB) are linearized when sampled
  // (piecewise-linear ~2.2 curve): sample them through an sRGB view.
  bool gamma = IsGammaFetch(fetch) && REXCVAR_GET(native_texture_gamma) == 1;
  DXGI_FORMAT srv_format = decoded.format;
  DXGI_FORMAT resource_format = decoded.format;
  if (gamma) {
    switch (decoded.format) {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        resource_format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        srv_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        break;
      case DXGI_FORMAT_B8G8R8A8_UNORM:
        resource_format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
        srv_format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        break;
      case DXGI_FORMAT_BC1_UNORM:
        resource_format = DXGI_FORMAT_BC1_TYPELESS;
        srv_format = DXGI_FORMAT_BC1_UNORM_SRGB;
        break;
      case DXGI_FORMAT_BC2_UNORM:
        resource_format = DXGI_FORMAT_BC2_TYPELESS;
        srv_format = DXGI_FORMAT_BC2_UNORM_SRGB;
        break;
      case DXGI_FORMAT_BC3_UNORM:
        resource_format = DXGI_FORMAT_BC3_TYPELESS;
        srv_format = DXGI_FORMAT_BC3_UNORM_SRGB;
        break;
      default:
        break;
    }
  }
  desc.Format = resource_format;
  desc.SampleDesc.Count = 1;
  if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&entry.resource)))) {
    return 0;
  }
  uint32_t subresources = decoded.mip_levels * (volume ? 1 : decoded.depth);
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(subresources);
  std::vector<UINT> rows(subresources);
  std::vector<UINT64> row_sizes(subresources);
  UINT64 total = 0;
  device_->GetCopyableFootprints(&desc, 0, subresources, 0, layouts.data(), rows.data(),
                                 row_sizes.data(), &total);
  UploadAlloc upload;
  if (!Upload(size_t(total), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, upload)) {
    entry.resource.Reset();
    return 0;
  }
  UINT64 upload_base = upload.offset;
  if (volume) {
    // One subresource; the footprint holds all depth slices.
    for (uint32_t z = 0; z < decoded.depth; ++z) {
      const auto& level = decoded.levels[z];
      uint8_t* dst = upload.cpu + layouts[0].Offset +
                     size_t(z) * layouts[0].Footprint.RowPitch * rows[0];
      uint32_t copy_rows = std::min<uint32_t>(rows[0], level.rows);
      size_t copy_bytes = std::min<size_t>(size_t(row_sizes[0]), level.row_pitch);
      for (uint32_t r = 0; r < copy_rows; ++r) {
        std::memcpy(dst + size_t(r) * layouts[0].Footprint.RowPitch,
                    decoded.data.data() + level.offset + size_t(r) * level.row_pitch, copy_bytes);
      }
    }
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = layouts[0];
    src.PlacedFootprint.Offset += upload_base;
    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = entry.resource.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;
    command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src, nullptr);
  }
  for (uint32_t mip = 0; mip < decoded.mip_levels && !volume; ++mip) {
    for (uint32_t slice = 0; slice < decoded.depth; ++slice) {
      // Decoder order: mip-major, slice-minor. D3D12 subresource: mip + slice * mips.
      const auto& level = decoded.levels[size_t(mip) * decoded.depth + slice];
      uint32_t sub = mip + slice * decoded.mip_levels;
      uint8_t* dst = upload.cpu + layouts[sub].Offset;
      uint32_t copy_rows = std::min<uint32_t>(rows[sub], level.rows);
      size_t copy_bytes = std::min<size_t>(size_t(row_sizes[sub]), level.row_pitch);
      for (uint32_t r = 0; r < copy_rows; ++r) {
        std::memcpy(dst + size_t(r) * layouts[sub].Footprint.RowPitch,
                    decoded.data.data() + level.offset + size_t(r) * level.row_pitch, copy_bytes);
      }
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource = upload.resource;
      src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint = layouts[sub];
      src.PlacedFootprint.Offset += upload_base;
      D3D12_TEXTURE_COPY_LOCATION dst_loc{};
      dst_loc.pResource = entry.resource.Get();
      dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst_loc.SubresourceIndex = sub;
      command_list_->CopyTextureRegion(&dst_loc, 0, 0, 0, &src, nullptr);
    }
  }
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
  Transition(entry.resource.Get(), state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = srv_format;
  srv.Shader4ComponentMapping = decoded.component_mapping;
  if (cube) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.TextureCube.MipLevels = decoded.mip_levels;
  } else if (volume) {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = decoded.mip_levels;
  } else {
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = decoded.mip_levels;
  }
  entry.srv_index = AllocSrvIndex();
  if (!entry.srv_index) {
    Retire(entry.resource);
    entry.resource.Reset();
    return 0;
  }
  device_->CreateShaderResourceView(
      entry.resource.Get(), &srv,
      provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                      entry.srv_index));
  entry.guest_base = decoded.base_address;
  entry.guest_size = decoded.base_size;
  entry.guest_format = decoded.guest_format;
  if (pre_armed && pre_base == entry.guest_base && pre_size == entry.guest_size) {
    entry.watch_seq = pre_seq;
    entry.content_hash = pre_hash;
  } else {
    if (texture_watch_ && entry.guest_size) {
      entry.watch_seq = ArmTextureWatch(entry.guest_base, entry.guest_size);
    }
    entry.content_hash =
        entry.guest_size
            ? XXH3_64bits(REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(entry.guest_base),
                          entry.guest_size)
            : 0;
  }
  entry.checked_frame = entry.hashed_frame = frame_count_;
  ++stats_.textures_created;
  return entry.srv_index;
}

uint32_t Renderer::GetSamplerIndex(const uint32_t fetch[6], bool force_linear, float lod_bias) {
  xenos::xe_gpu_texture_fetch_t f;
  std::memcpy(&f, fetch, sizeof(f));
  uint32_t bias_q = uint32_t(std::clamp(lod_bias, 0.0f, 7.9f) * 16.0f);  // 1/16 steps
  uint64_t key = (uint64_t(fetch[0]) & 0x7FFC00) | (uint64_t(fetch[3] & 0x3FF80000) << 32) |
                 (uint64_t(force_linear) << 63) | (uint64_t(bias_q) << 1);
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }
  D3D12_SAMPLER_DESC desc{};
  bool mag_linear = uint32_t(f.mag_filter) == 1 || force_linear;
  bool min_linear = uint32_t(f.min_filter) == 1 || force_linear;
  bool mip_linear = uint32_t(f.mip_filter) == 1;
  uint32_t aniso = uint32_t(f.aniso_filter);
  uint32_t max_aniso = aniso > 1 ? std::min(16u, 1u << (aniso - 1)) : 1u;
  // anisotropic_filtering setting: overrides filtered (linear min/mag)
  // textures; point-sampled lookups stay point-sampled.
  if (anisotropy_ >= 1 && (min_linear || mag_linear || aniso > 1)) {
    max_aniso = uint32_t(std::min(anisotropy_, 16));
  }
  if (max_aniso > 1) {
    desc.Filter = D3D12_FILTER_ANISOTROPIC;
    desc.MaxAnisotropy = max_aniso;
  } else if (aniso > 1) {
    // Game asked for anisotropy, the setting turned it off: trilinear.
    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    desc.MaxAnisotropy = 1;
  } else {
    desc.Filter = D3D12_ENCODE_BASIC_FILTER(
        min_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        mag_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        mip_linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
        D3D12_FILTER_REDUCTION_TYPE_STANDARD);
    desc.MaxAnisotropy = 1;
  }
  desc.AddressU = MapClamp(uint32_t(f.clamp_x));
  desc.AddressV = MapClamp(uint32_t(f.clamp_y));
  desc.AddressW = MapClamp(uint32_t(f.clamp_z));
  desc.MaxLOD = D3D12_FLOAT32_MAX;
  desc.MipLODBias = float(bias_q) / 16.0f;
  desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  uint32_t index = sampler_heap_next_ < kSamplerHeapSize ? sampler_heap_next_++ : 0;
  device_->CreateSampler(&desc, provider_->OffsetSamplerDescriptor(
                                    sampler_heap_->GetCPUDescriptorHandleForHeapStart(), index));
  samplers_[key] = index;
  return index;
}

void Renderer::OnGpuSwap(uint64_t swap_number, const uint32_t* regs, uint32_t count) {
  if (!REXCVAR_GET(native_ab_mode) || REXCVAR_GET(native_dump_swap) <= 0 ||
      swap_number != uint64_t(REXCVAR_GET(native_dump_swap))) {
    return;
  }
  std::vector<std::array<uint32_t, 6>> fetches;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // Mirror vs Xenos register file at the same guest swap.
    if (!mirror_snapshot_.empty() && regs) {
      struct Range { const char* name; uint32_t first, last; };
      const Range ranges[] = {{"vs consts", 0x4000, 0x4400}, {"ps consts", 0x4400, 0x4800},
                              {"fetch", 0x4800, 0x4900}, {"bool/loop", 0x4900, 0x4928},
                              {"regs", 0x2000, 0x3000}};
      for (const Range& r : ranges) {
        uint32_t diffs = 0, unwritten = 0;
        std::string first;
        for (uint32_t i = r.first; i < r.last && i < count; ++i) {
          if (mirror_snapshot_[i] == regs[i]) continue;
          ++diffs;
          if (!mirror_.written(i)) ++unwritten;
          if (diffs <= 12) first += fmt::format(" {:04X}:{:08X}/{:08X}", i, mirror_snapshot_[i], regs[i]);
        }
        REXLOG_INFO("native: A/B regs {}: {} differ ({} never written by mirror){}", r.name, diffs,
                    unwritten, first);
      }
    }
    for (auto& [key, rt] : resolve_textures_) {
      std::array<uint32_t, 6> f;
      std::memcpy(f.data(), rt.guest_fetch, sizeof(rt.guest_fetch));
      if (f[1]) fetches.push_back(f);
    }
  }
  std::string dir = REXCVAR_GET(native_dump_dir);
  int written = 0;
  for (auto& f : fetches) {
    DecodedTexture t;
    const char* why = nullptr;
    if (!DecodeTexture(f.data(), t, &why) || t.levels.empty()) {
      REXLOG_INFO("native: xenos dump skip {:08X}: {}", f[1], why ? why : "?");
      continue;
    }
    const auto& l = t.levels[0];
    {
      char raw_name[128];
      std::snprintf(raw_name, sizeof(raw_name), "%s/xenosmem_%08X_gf%u_%ux%u.bin", dir.c_str(),
                    GuestPhysical(f[1] & 0xFFFFF000u), f[1] & 0x3F, t.width, t.height);
      if (std::FILE* file = std::fopen(raw_name, "wb")) {
        std::fwrite(f.data(), 4, 6, file);
        std::fwrite(REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(t.base_address), 1,
                    t.base_size, file);
        std::fclose(file);
      }
    }
    char name[128];
    std::snprintf(name, sizeof(name), "%s/xenos_%08X_gf%u_%ux%u.raw", dir.c_str(),
                  GuestPhysical(f[1] & 0xFFFFF000u), f[1] & 0x3F, t.width, t.height);
    if (std::FILE* file = std::fopen(name, "wb")) {
      uint32_t header[4] = {t.width, t.height, uint32_t(t.format), l.row_pitch};
      std::fwrite(header, 4, 4, file);
      std::fwrite(t.data.data() + l.offset, 1, size_t(l.row_pitch) * l.rows, file);
      std::fclose(file);
      ++written;
    }
  }
  REXLOG_INFO("native: dumped {} Xenos resolve results at GPU swap {}", written, swap_number);
}

void Renderer::Breadcrumb(const char* what) {
  // Debug only: a MARKER_OUT WriteBufferImmediate after every draw makes the GPU
  // drain its pipeline ~600 times per frame.
  if (!REXCVAR_GET(native_gpu_breadcrumbs)) return;
  uint32_t seq = ++crumb_seq_;
  CrumbDesc& d = crumbs_[seq & 4095];
  d.what = what;
  d.prim = crumb_prim_;
  d.count = crumb_count_;
  d.seq = seq;
  d.pass = g_current_pass;
  d.vs = current_vs_hash_;
  d.ps = current_ps_hash_;
  d.frame = frame_count_;
  if (!crumb_buffer_) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&crumb_buffer_)))) {
      return;
    }
    void* p = nullptr;
    crumb_buffer_->Map(0, nullptr, &p);
    crumb_cpu_ = static_cast<volatile uint32_t*>(p);
  }
  if (!crumb_list_ && FAILED(command_list_.As(&crumb_list_))) return;
  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param{crumb_buffer_->GetGPUVirtualAddress(), seq};
  D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
  crumb_list_->WriteBufferImmediate(1, &param, &mode);
}

void Renderer::ReportHang() {
  if (!crumb_cpu_) return;
  uint32_t done = crumb_cpu_[0];
  REXLOG_ERROR("native: GPU hang diagnostics: last completed draw #{} of {} recorded", done,
               crumb_seq_);
  for (uint32_t k = done; k <= done + 4 && k <= crumb_seq_; ++k) {
    const CrumbDesc& d = crumbs_[k & 4095];
    if (d.seq != k) continue;
    REXLOG_ERROR("native:   {} {} #{} frame {} pass {} prim {} count {} vs {:016X} ps {:016X}",
                 k == done ? "done" : "pending", d.what, k, d.frame, d.pass, d.prim, d.count, d.vs,
                 d.ps);
  }
  rex::FlushLogging();
  for (int i = 0; i < 6; ++i) {
    Sleep(500);
    REXLOG_ERROR("native: hang watch {}: crumb {} fence {} removed {:08X}", i, crumb_cpu_[0],
                 fence_->GetCompletedValue(), uint32_t(device_->GetDeviceRemovedReason()));
  }
}

uint32_t Renderer::GpuConstant(uint8_t* base, uint32_t dev, uint32_t reg,
                               uint32_t shadow_offset) const {
  if (REXCVAR_GET(native_pm4_mirror) && mirror_.written(reg)) return mirror_.reg(reg);
  return Load32(base, dev + shadow_offset);
}

void Renderer::NoteRingConstants(bool pixel, uint32_t start, uint32_t count, uint32_t ring_ptr) {
  if (REXCVAR_GET(native_pm4_mirror)) return;  // the mirror parses the packets itself
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (trace_state_ == 1) {
    REXLOG_INFO("trace p{} ring consts {} start {} count {} ptr {:08X}", g_current_pass,
                pixel ? "ps" : "vs", start, count, ring_ptr);
  }
  if (ring_ptr && count && start + count <= 256) {
    pending_ring_constants_.push_back({pixel, start, count, ring_ptr});
  }
}

void Renderer::ApplyLoadAluConstants(uint8_t* base, uint32_t dev, uint32_t table,
                                     uint32_t data) {
  if (REXCVAR_GET(native_pm4_mirror)) return;  // the mirror parses the packet itself
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // Earlier ring writes precede this load on the GPU.
  FlushRingConstants(base, dev);
  uint32_t rel = Load32(base, table + 20);
  if (!rel || !data) return;
  uint32_t header = table + rel;
  uint32_t p = header + 20;
  uint32_t end = p + Load32(base, header + 16);
  while (p < end) {
    uint32_t reg = Load16(base, p);
    uint32_t dwords = Load16(base, p + 2);
    if (!dwords) break;
    uint32_t offset = Load32(base, p + 4);
    p += 8;
    // VS constants 0-255 and PS constants 256-511 are contiguous in the shadow.
    if (reg * 4 + dwords <= 512 * 4) {
      // The packet carries the physical address (0xE0000000+ views are offset by 4 KB).
      const uint8_t* src =
          REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(GuestPhysical(data + offset));
      std::memcpy(base + dev + kDevVsConstants + reg * 16, src, dwords * 4);
    }
  }
}

void Renderer::FlushRingConstants(uint8_t* base, uint32_t dev) {
  if (!REXCVAR_GET(native_ring_constants) || REXCVAR_GET(native_pm4_mirror)) {
    pending_ring_constants_.clear();
    return;
  }
  // Keep the device's cached constants equal to what the GPU would receive
  // (the XDK contract for GpuBeginShaderConstantF4's cached pointer).
  for (const RingConstants& rc : pending_ring_constants_) {
    uint32_t dst = dev + (rc.pixel ? kDevPsConstants : kDevVsConstants) + rc.start * 16;
    std::memcpy(base + dst, base + rc.ring_ptr, size_t(rc.count) * 16);
  }
  pending_ring_constants_.clear();
}

bool Renderer::UploadConstants(uint8_t* base, uint32_t dev) {
  FlushRingConstants(base, dev);
  UploadAlloc shared;
  if (!Upload(512, 256, shared)) {
    return false;
  }
  bool use_mirror = REXCVAR_GET(native_pm4_mirror);
  // Debug: native_debug_ps_const=<ps hash hex>:<register>:<x>,<y>,<z>,<w>
  static const struct Override {
    uint64_t hash = 0;
    uint32_t reg = 0;
    float v[4] = {};
  } ps_override = [] {
    Override o;
    std::string spec = REXCVAR_GET(native_debug_ps_const);
    unsigned long long h = 0;
    if (!spec.empty() &&
        std::sscanf(spec.c_str(), "%llx:%u:%f,%f,%f,%f", &h, &o.reg, &o.v[0], &o.v[1], &o.v[2],
                    &o.v[3]) == 6) {
      o.hash = h;
    }
    return o;
  }();
  bool override_ps = ps_override.hash && ps_override.hash == current_ps_hash_ && ps_override.reg < 256;
  // NaN constants become 0: the game leaves uninitialized lanes (e.g. light
  // c64.w) that Xenos multiplies away (0 * NaN = 0 in its ALU); HLSL keeps the
  // NaN (same as the Xenos-path E041 sanitizer). Source: the PM4 mirror (GPU
  // truth, host order) or the byte-swapped device shadow.
  auto fill = [&](uint32_t mirror_base, uint32_t shadow_offset, uint32_t* dst) {
    if (use_mirror) {
      const float* src = reinterpret_cast<const float*>(mirror_.regs() + mirror_base);
      float* out = reinterpret_cast<float*>(dst);
      for (uint32_t i = 0; i < 1024; i += 4) {
        __m128 v = _mm_loadu_ps(src + i);
        _mm_storeu_ps(out + i, _mm_andnot_ps(_mm_cmpunord_ps(v, v), v));
      }
      return;
    }
    const uint32_t* src = reinterpret_cast<const uint32_t*>(base + dev + shadow_offset);
    for (uint32_t i = 0; i < 1024; ++i) {
      uint32_t v = __builtin_bswap32(src[i]);
      dst[i] = ((v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu)) ? 0u : v;
    }
  };
  if (!use_mirror || cb_vs_version_ != mirror_.vs_version || !cb_vs_gpu_) {
    UploadAlloc vs;
    if (!Upload(4096, 256, vs)) return false;
    fill(Pm4Mirror::kAluConstantBase, kDevVsConstants, reinterpret_cast<uint32_t*>(vs.cpu));
    cb_vs_gpu_ = vs.gpu;
    cb_vs_version_ = use_mirror ? mirror_.vs_version : 0;
  }
  if (!use_mirror || override_ps || cb_ps_version_ != mirror_.ps_version || !cb_ps_gpu_) {
    UploadAlloc ps;
    if (!Upload(4096, 256, ps)) return false;
    uint32_t* ps_dst = reinterpret_cast<uint32_t*>(ps.cpu);
    fill(Pm4Mirror::kAluConstantBase + 1024, kDevPsConstants, ps_dst);
    if (override_ps) std::memcpy(ps_dst + ps_override.reg * 4, ps_override.v, 16);
    cb_ps_gpu_ = ps.gpu;
    cb_ps_version_ = (use_mirror && !override_ps) ? mirror_.ps_version : 0;
  }
  uint32_t* s = reinterpret_cast<uint32_t*>(shared.cpu);
  std::memset(s, 0, 512);
  for (uint32_t slot = 0; slot < 16; ++slot) {
    uint32_t fetch[6];
    for (uint32_t d = 0; d < 6; ++d) {
      fetch[d] = GpuConstant(base, dev, Pm4Mirror::kFetchConstantBase + 6 * slot + d,
                             kDevFetchConstants + 24 * slot + 4 * d);
    }
    uint32_t srv = 0, sampler = 0;
    if ((fetch[0] & 3) == uint32_t(xenos::FetchConstantType::kTexture) &&
        Load32(base, dev + kDevTextures + 4 * slot)) {
      SlotCache& sc = slot_cache_[slot];
      // Same fetch as the previous draw in this frame: the SRV cannot have
      // changed (resolves invalidate the cache below).
      if (sc.frame == frame_count_ && !std::memcmp(sc.fetch, fetch, sizeof(sc.fetch))) {
        srv = sc.srv;
      } else {
        srv = GetTextureSrvIndex(base, fetch);
        // Resolve textures rendered at render_scale / shadow_quality: bits
        // 29-30 carry scale - 1 so the shader keeps texel offsets and filter
        // weights in guest texels (TEX_SCALE in the XenosRecomp patch).
        auto latest = resolve_latest_.find(GuestPhysical(fetch[1] & 0xFFFFF000u));
        if (srv > 2 && latest != resolve_latest_.end()) {
          auto rt = resolve_textures_.find(latest->second);
          // Not for the shadow atlas: its receivers run the whole PCF kernel
          // on the host texel grid (g_ShadowAtlasTexelScale scales the baked
          // tap offsets instead, EXP-041).
          if (rt != resolve_textures_.end() && rt->second.scale != 1.0f &&
              (!rt->second.shadow || REXCVAR_GET(native_shadow_pcf_mode) == 1)) {
            // Bits 16-27: scale in 8.8 fixed point (TEX_SCALE in the
            // XenosRecomp patch); SRV indices use bits 0-14.
            uint32_t fixed = uint32_t(std::lround(double(rt->second.scale) * 256.0));
            srv |= std::min(fixed, 0xFFFu) << 16;
            // Point fetches of an upscaled color target (post-process chains:
            // glow, bloom downsamples) sample linearly: at the guest texel
            // positions the game samples, bilinear averages exactly the host
            // texels covering that guest texel. Point sampling picked one of
            // them and turned small bright details into square/striped
            // patterns (EXP-045). Depth resolves (R32F) stay point-sampled.
            sc.smooth = rt->second.format != DXGI_FORMAT_R32_FLOAT;
          } else {
            sc.smooth = false;
          }
        } else {
          sc.smooth = false;
        }
        std::memcpy(sc.fetch, fetch, sizeof(sc.fetch));
        sc.srv = srv;
        sc.frame = frame_count_;
      }
      sampler = GetSamplerIndex(fetch, sc.smooth);
      // Smooth effects: small textures of blended draws (particles, glows,
      // light halos) magnify with a cubic B-spline above the original
      // resolution (bit 28, tfetch2D in the XenosRecomp patch).
      // Their mip level is biased by log2(scale) so they keep the texel
      // density - and the soft look - they had at the original resolution:
      // the finest mips of those wispy textures read as streaks when magnified.
      if (smooth_effects_ && bound_scale_ > 1.01f && srv > 2 && !(srv & 0xFFF0000u) &&
          (fetch[2] & 0x1FFF) < 256 && ((fetch[2] >> 13) & 0x1FFF) < 256) {
        uint32_t blend = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0);
        if ((blend & 0x1FFF) != 0x0001) {
          srv |= 0x10000000u;
          sampler = GetSamplerIndex(fetch, sc.smooth, std::log2(bound_scale_));
        }
      }
      // PWL gamma in the shader (XenosRecomp CONAN_RECOMP tfetch*).
      if (srv > 2 && REXCVAR_GET(native_texture_gamma) == 2 && IsGammaFetch(fetch)) {
        srv |= 0x80000000u;
      }
    }
    if (trace_state_ == 1 && (fetch[0] & 3) == uint32_t(xenos::FetchConstantType::kTexture)) {
      uint32_t addr = GuestPhysical(fetch[1] & 0xFFFFF000u);
      auto latest = resolve_latest_.find(addr);
      std::string src = "texture";
      if (latest != resolve_latest_.end()) {
        auto rt = resolve_textures_.find(latest->second);
        if (rt != resolve_textures_.end()) {
          src = fmt::format("resolve {}x{} dxgi {}", rt->second.width, rt->second.height,
                            int(rt->second.srv_format));
        }
      }
      if (src == "texture") {
        for (auto& [k, te] : textures_) {
          if ((te.srv_index | 0x80000000u) == (srv | 0x80000000u) && te.resource) {
            D3D12_RESOURCE_DESC rd = te.resource->GetDesc();
            src = fmt::format("texture {}x{}x{} dxgi {} mips {} guest {:08X}+{:X}", rd.Width, rd.Height,
                              rd.DepthOrArraySize, int(rd.Format), rd.MipLevels, te.guest_base,
                              te.guest_size);
            break;
          }
        }
      }
      REXLOG_INFO("trace   slot {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} fmt {} srv {:X} ({})",
                  slot, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], fetch[1] & 0x3F,
                  srv, src);
    }
    // Each dimension reads its own descriptor heap view; the others get nulls.
    uint32_t dimension = (fetch[5] >> 9) & 3;
    s[slot] = dimension <= 1 ? srv : 0;
    s[16 + slot] = dimension == 2 && srv ? srv : 1;
    s[32 + slot] = dimension == 3 && srv ? srv : 2;
    s[48 + slot] = sampler;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    s[64 + i] = GpuConstant(base, dev, Pm4Mirror::kBoolConstantBase + i, kDevVsBools + 4 * i);
    s[68 + i] = GpuConstant(base, dev, Pm4Mirror::kBoolConstantBase + 4 + i, kDevPsBools + 4 * i);
  }
  s[72] = 0;
  // g_HalfPixelOffset (c18.yz): Xenos D3D pixel centers (PA_SU_VTX_CNTL
  // pix_center = 0) are emulated like Xenia, by moving the viewport half a
  // pixel right/down (the VS adds g_HalfPixelOffset * w to the position).
  float half_pixel[2] = {0.0f, 0.0f};
  if (REXCVAR_GET(native_half_pixel_offset) &&
      !(LoadReg(base, dev, rex::graphics::XE_GPU_REG_PA_SU_VTX_CNTL) & 1)) {
    float w = 0.0f, h = 0.0f;
    if (ScreenSpaceDraw(base, dev)) {
      ScreenSpaceTargetSize(w, h);
    } else {
      w = LoadF32(base, dev + kDevViewport + 8);
      h = LoadF32(base, dev + kDevViewport + 12);
      if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
        w = float(kOutputWidth);
        h = float(kOutputHeight);
      }
    }
    half_pixel[0] = 1.0f / w;
    half_pixel[1] = -1.0f / h;
  }
  std::memcpy(&s[73], &half_pixel[0], 4);
  std::memcpy(&s[74], &half_pixel[1], 4);
  // Runtime spec constants (shared c28.x, see the XenosRecomp patch):
  // bit 1 = alpha test (RB_COLORCONTROL), threshold from RB_ALPHA_REF.
  // The translated test is clip(alpha - ref): exact for GREATER/GEQUAL.
  uint32_t color_control = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
  uint32_t alpha_func = color_control & 7;
  uint32_t spec = 0;
  float alpha_ref = 0.0f;
  if ((color_control & 0x8) && alpha_func != 7) {
    spec |= 1u << 1;
    uint32_t ref = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_ALPHA_REF);
    std::memcpy(&alpha_ref, &ref, 4);
    if (alpha_func == 0) alpha_ref = 2.0f;  // NEVER: discard everything
    static bool logged_func[8] = {};
    if (alpha_func != 4 && alpha_func != 6 && alpha_func != 0 && !logged_func[alpha_func]) {
      logged_func[alpha_func] = true;
      REXLOG_WARN("native: alpha test func {} approximated as GEQUAL", alpha_func);
    }
  }
  std::memcpy(&s[75], &alpha_ref, 4);
  // Foliage antialiasing: the alpha test becomes alpha-to-coverage
  // (SPEC_CONSTANT_ALPHA_TO_COVERAGE, mip-corrected, see the PSO side).
  // Soft particles: blended, depth-tested, non-depth-writing draws of the
  // transparent pass fade out near the scene surface behind them.
  if (soft_particles_ && camera_valid_ && scene_depth_key_ && g_current_pass == kPassRenderSorted &&
      bound_rts_[0] && !ScreenSpaceDraw(base, dev)) {
    uint32_t blend = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0);
    uint32_t depthctl = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL);
    auto rt_it = resolve_textures_.find(scene_depth_key_);
    if ((blend & 0x1FFF) != 0x0001 && (depthctl & 0x2) && !(depthctl & 0x4) &&
        rt_it != resolve_textures_.end()) {
      ResolveTexture& depth = rt_it->second;
      if (!depth.plain_srv) {
        depth.plain_srv = AllocSrvIndex();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = depth.srv_format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(
            depth.resource.Get(), &srv,
            provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            depth.plain_srv));
      }
      if (depth.plain_srv) {
        float w_column[2] = {inv_view_proj_[11], inv_view_proj_[15]};
        float distance = float(REXCVAR_GET(soft_particle_distance));
        float rt_width = float(HostPx(bound_rts_[0]->width, bound_rts_[0]->scale));
        float texel_scale = float(HostPx(depth.width, depth.scale)) / std::max(rt_width, 1.0f);
        std::memcpy(&s[116], w_column, 8);
        std::memcpy(&s[118], &distance, 4);
        std::memcpy(&s[119], &texel_scale, 4);
        s[120] = depth.plain_srv;
        // SRC_ALPHA source blend: fade alpha only; otherwise rgba.
        spec |= ((blend & 0x1F) == 6) ? (1u << 5) : (1u << 4);
      }
    }
  }
  if (AlphaTestToCoverage(base, dev)) {
    spec = (spec & ~(1u << 1)) | (1u << 3);
    ++stats_.a2c_draws[g_current_pass & 31];
  } else if (spec & (1u << 1)) {
    ++stats_.alpha_test_draws[g_current_pass & 31];
  }
  s[112] = spec;
  // g_PixelPosScale (c28.y): host pixel -> guest pixel for the pixel position
  // input (targets rendered at render_scale / shadow_quality).
  {
    float inv_scale = 1.0f / bound_scale_;
    std::memcpy(&s[113], &inv_scale, 4);
    // g_ShadowAtlasTexelScale (c28.z): shadow atlas PCF tap offsets.
    float atlas_scale =
        REXCVAR_GET(native_shadow_pcf_mode) == 1 ? 1.0f : 1.0f / shadow_scale_;
    std::memcpy(&s[114], &atlas_scale, 4);
    // g_ShadowSoftness (c28.w): shadow smoothing option (rotated, widened PCF).
    static const float kSoftness[3] = {0.0f, 1.0f, 1.6f};
    float softness = kSoftness[std::clamp(shadow_smoothing_, 0, 2)];
    std::memcpy(&s[115], &softness, 4);
  }
  for (uint32_t i = 0; i < 16; ++i) {
    s[76 + i] = GpuConstant(base, dev, Pm4Mirror::kLoopConstantBase + i, kDevVsLoops + 4 * i);
    s[92 + i] =
        GpuConstant(base, dev, Pm4Mirror::kLoopConstantBase + 16 + i, kDevPsLoops + 4 * i);
  }
  // g_ScreenXform (c27): screen-space draws (viewport transform disabled in
  // PA_CL_VTE_CNTL, e.g. the upscale pass) emit pixel positions; map them to NDC.
  float xform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
  if (ScreenSpaceDraw(base, dev)) {
    float w = 0, h = 0;
    ScreenSpaceTargetSize(w, h);
    uint32_t win = LoadReg(base, dev, rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
    // 15-bit signed X/Y window offsets.
    float wx = float(int32_t(win << 17) >> 17), wy = float(int32_t((win >> 16) << 17) >> 17);
    xform[0] = 2.0f / w;
    xform[1] = -2.0f / h;
    xform[2] = -1.0f + wx * xform[0];
    xform[3] = 1.0f + wy * xform[1];
  }
  std::memcpy(&s[108], xform, sizeof(xform));
  if (trace_state_ == 1) {
    const float* v = reinterpret_cast<const float*>(mirror_.regs() + Pm4Mirror::kAluConstantBase);
    std::string vc;
    for (int r = 160; r < 168; ++r)
      vc += fmt::format(" c{}=({} {} {} {})", r, v[r * 4], v[r * 4 + 1], v[r * 4 + 2], v[r * 4 + 3]);
    REXLOG_INFO("trace   vs{}", vc);
    const float* c = reinterpret_cast<const float*>(mirror_.regs() + Pm4Mirror::kAluConstantBase + 1024);
    REXLOG_INFO("trace   ps c160 ({} {} {} {}) c161 ({} {} {} {})", c[640], c[641], c[642], c[643],
                c[644], c[645], c[646], c[647]);
  }
  command_list_->SetGraphicsRootConstantBufferView(0, cb_vs_gpu_);
  command_list_->SetGraphicsRootConstantBufferView(1, cb_ps_gpu_);
  command_list_->SetGraphicsRootConstantBufferView(2, shared.gpu);
  return true;
}

bool Renderer::AlphaTestToCoverage(uint8_t* base, uint32_t dev) {
  // foliage_antialiasing: opaque (non-blended) alpha-tested draws into a
  // multisampled target get alpha-to-coverage instead of a hard alpha test:
  // leaves, grass and grilles get antialiased edges from the game's 4x MSAA.
  if (!foliage_aa_ || bound_samples_ <= 1) return false;
  uint32_t color_control = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLORCONTROL);
  uint32_t func = color_control & 7;
  if (!(color_control & 0x8) || (func != 4 && func != 6)) return false;  // GREATER / GEQUAL
  uint32_t blend = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0);
  // color and alpha: ONE, ZERO, ADD (no blending)
  return (blend & 0x1FFF) == 0x0001 && (blend & 0x1FFF0000) == 0x00010000;
}

bool Renderer::ScreenSpaceDraw(uint8_t* base, uint32_t dev) {
  // VPORT_X_SCALE_ENA clear: the vertex shader output is in window coordinates.
  return !(LoadReg(base, dev, rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL) & 1u);
}

void Renderer::ScreenSpaceTargetSize(float& w, float& h) {
  HostSurface* t = bound_rts_[0] ? bound_rts_[0] : bound_ds_;
  w = t ? float(t->width) : float(kOutputWidth);
  h = t ? float(t->height) : float(kOutputHeight);
}

void Renderer::ApplyFixedFunctionState(uint8_t* base, uint32_t dev) {
  // The device viewport (SetViewportF) stores X, Y, Width, Height, MinZ, MaxZ
  // as floats.
  D3D12_VIEWPORT vp{};
  vp.TopLeftX = LoadF32(base, dev + kDevViewport + 0);
  vp.TopLeftY = LoadF32(base, dev + kDevViewport + 4);
  vp.Width = LoadF32(base, dev + kDevViewport + 8);
  vp.Height = LoadF32(base, dev + kDevViewport + 12);
  vp.MinDepth = LoadF32(base, dev + kDevViewport + 16);
  vp.MaxDepth = LoadF32(base, dev + kDevViewport + 20);
  if (vp.Width <= 0 || vp.Height <= 0 || vp.Width > 8192 || vp.Height > 8192) {
    vp.TopLeftX = vp.TopLeftY = 0;
    vp.Width = kOutputWidth;
    vp.Height = kOutputHeight;
  }
  vp.MinDepth = std::clamp(vp.MinDepth, 0.0f, 1.0f);
  vp.MaxDepth = std::clamp(vp.MaxDepth, 0.0f, 1.0f);
  if (ScreenSpaceDraw(base, dev)) {
    // Positions are already in pixels (see g_ScreenXform); cover the target.
    vp.TopLeftX = vp.TopLeftY = 0;
    ScreenSpaceTargetSize(vp.Width, vp.Height);
  }
  if (trace_state_ == 1) {
    auto regf = [&](uint32_t r) {
      uint32_t v = LoadReg(base, dev, r);
      float f;
      std::memcpy(&f, &v, 4);
      return f;
    };
    REXLOG_INFO("trace   vp {} {} {}x{} z {}..{} | vport zscale {} zoffset {} vte {:08X} clip {:08X} "
                "mirror zscale {} zoffset {} stencilrefmask {:08X} (mirror {:08X}) depthctl mirror {:08X} "
                "surface_info {:08X} color_info {:08X} depth_info {:08X} modecontrol {:08X}",
                vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth,
                regf(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE),
                regf(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_PA_CL_CLIP_CNTL),
                [&] { uint32_t v = mirror_.reg(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE); float f; std::memcpy(&f, &v, 4); return f; }(),
                [&] { uint32_t v = mirror_.reg(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET); float f; std::memcpy(&f, &v, 4); return f; }(),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_STENCILREFMASK),
                mirror_.reg(rex::graphics::XE_GPU_REG_RB_STENCILREFMASK),
                mirror_.reg(rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL), mirror_.reg(0x2000),
                mirror_.reg(0x2001), mirror_.reg(0x2002), mirror_.reg(0x2208));
  }
  if (bound_scale_ != 1.0f) {
    float k = bound_scale_;
    vp.TopLeftX *= k;
    vp.TopLeftY *= k;
    vp.Width *= k;
    vp.Height *= k;
  }
  command_list_->RSSetViewports(1, &vp);
  // Scissor (draw_util::GetScissor): window scissor + window offset, clamped by
  // the screen scissor. Not during predicated tiling: host surfaces are untiled
  // while the XDK sets a per-tile scissor.
  D3D12_RECT scissor{0, 0, 16384, 16384};
  if (REXCVAR_GET(native_scissor) && !tiling_active_) {
    uint32_t tl = mirror_.reg(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL);
    uint32_t br = mirror_.reg(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR);
    int32_t x0 = int32_t(tl & 0x7FFF), y0 = int32_t((tl >> 16) & 0x7FFF);
    int32_t x1 = int32_t(br & 0x7FFF), y1 = int32_t((br >> 16) & 0x7FFF);
    if (!(tl >> 31)) {
      uint32_t off = mirror_.reg(rex::graphics::XE_GPU_REG_PA_SC_WINDOW_OFFSET);
      int32_t ox = int32_t(off << 17) >> 17, oy = int32_t((off >> 16) << 17) >> 17;
      x0 += ox;
      y0 += oy;
      x1 += ox;
      y1 += oy;
    }
    uint32_t stl = mirror_.reg(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL);
    uint32_t sbr = mirror_.reg(rex::graphics::XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR);
    if (sbr) {
      x0 = std::max(x0, int32_t(stl & 0x7FFF));
      y0 = std::max(y0, int32_t((stl >> 16) & 0x7FFF));
      x1 = std::min(x1, int32_t(sbr & 0x7FFF));
      y1 = std::min(y1, int32_t((sbr >> 16) & 0x7FFF));
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::max(x1, x0);
    y1 = std::max(y1, y0);
    if (br) {
      scissor = ScaleRect({LONG(x0), LONG(y0), LONG(x1), LONG(y1)}, bound_scale_);
    }
    if (trace_state_ == 1) {
      REXLOG_INFO("trace   scissor tl {:08X} br {:08X} -> {},{}-{},{}", tl, br, scissor.left,
                  scissor.top, scissor.right, scissor.bottom);
    }
  }
  command_list_->RSSetScissorRects(1, &scissor);
  uint32_t stencil_ref_mask = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_STENCILREFMASK);
  command_list_->OMSetStencilRef(stencil_ref_mask & 0xFF);
  float blend_factor[4];
  for (uint32_t i = 0; i < 4; ++i) {
    uint32_t v = LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_BLEND_RED + i);
    std::memcpy(&blend_factor[i], &v, 4);
  }
  command_list_->OMSetBlendFactor(blend_factor);
}

bool Renderer::PrepareDraw(uint8_t* base, uint32_t dev, uint32_t prim,
                           D3D12_PRIMITIVE_TOPOLOGY& topology, bool& quads,
                           D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut) {
  static const uint64_t pass_mask = [] {
    std::string list = REXCVAR_GET(native_pass_mask);
    if (list.empty()) return ~0ull;
    uint64_t mask = 0;
    size_t pos = 0;
    while (pos < list.size()) {
      size_t comma = list.find(',', pos);
      if (comma == std::string::npos) comma = list.size();
      mask |= 1ull << std::stoi(list.substr(pos, comma - pos));
      pos = comma + 1;
    }
    return mask;
  }();
  if (!(pass_mask & (1ull << (g_current_pass & 63)))) {
    return false;
  }
  static const std::array<int, 3> skip = [] {
    std::array<int, 3> r{-1, 0, -1};
    std::sscanf(REXCVAR_GET(native_skip_draws).c_str(), "%d:%d-%d", &r[0], &r[1], &r[2]);
    return r;
  }();
  uint32_t draw_index = pass_draw_index_[g_current_pass & 31]++;
  if (g_current_pass == skip[0] && int(draw_index) >= skip[1] && int(draw_index) <= skip[2]) {
    return false;
  }
  uint64_t vs_hash = 0, ps_hash = 0;
  for (uint32_t off : {kDevShaderA, kDevShaderB}) {
    if (const GuestShaderInfo* info = LookupGuestShader(Load32(base, dev + off))) {
      (info->is_vertex ? vs_hash : ps_hash) = info->container_hash;
    }
  }
  uint32_t decl = Load32(base, dev + kDevVertexDecl);
  static const uint64_t skip_ps = [] {
    std::string v = REXCVAR_GET(native_skip_ps);
    return v.empty() ? 0ull : std::stoull(v, nullptr, 16);
  }();
  if (skip_ps && ps_hash == skip_ps) return false;
  if (trace_state_ == 1) {
    uint32_t n = decl ? Load32(base, decl + 0x18) : 0;
    std::string els;
    for (uint32_t i = 0; i < n && i < 16; ++i) {
      uint32_t e = decl + 0x34 + 12 * i;
      uint32_t st = Load16(base, e);
      if (st == 0xFF) break;
      els += fmt::format(" s{}o{}t{:X}u{}.{}", st,
                         Load16(base, e + 2),
                         Load32(base, e + 4), Load8(base, e + 9), Load8(base, e + 10));
    }
    std::string tex;
    for (uint32_t slot = 0; slot < 10; ++slot) {
      uint32_t f0 = Load32(base, dev + kDevFetchConstants + 24 * slot);
      uint32_t f1 = Load32(base, dev + kDevFetchConstants + 24 * slot + 4);
      uint32_t f3 = Load32(base, dev + kDevFetchConstants + 24 * slot + 12);
      if ((f0 & 3) == 2) tex += fmt::format(" t{}={:08X}/{:08X}/{:08X}", slot, f0, f1, f3);
    }
    uint32_t rt0 = Load32(base, dev + kDevRenderTargets);
    tex += fmt::format(" rt0info {:08X}", rt0 ? Load32(base, rt0 + 0x1C) : 0);
    els += tex;
    REXLOG_INFO("trace draw p{} #{} prim {} vs {:016X} ps {:016X} stride0 {} fetch0 {:08X} cc {:08X} blend {:08X} depth {:08X} mask {:X}{}",
                g_current_pass, draw_index, prim, vs_hash, ps_hash, Load8(base, dev + 0x30E8) * 4,
                Load32(base, dev + 0x778),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLORCONTROL),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_BLENDCONTROL0),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_DEPTHCONTROL),
                LoadReg(base, dev, rex::graphics::XE_GPU_REG_RB_COLOR_MASK), els);
  }
  bool depth_only = !ps_hash && vs_hash && Load32(base, dev + kDevShaderA) == 0;
  if (!vs_hash || (!ps_hash && !depth_only) || !decl) {
    ++stats_.skip_shader;
    stats_.pass_last_skip[g_current_pass & 31] = "shader";
    return false;
  }
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  quads = false;
  switch (static_cast<xenos::PrimitiveType>(prim)) {
    case xenos::PrimitiveType::kTriangleList:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      quads = true;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      break;
    case xenos::PrimitiveType::kPointList:
      topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      topology_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      break;
    case xenos::PrimitiveType::kRectangleList:
      // Expanded on the CPU into a triangle list by the caller.
      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
      break;
    default:
      ++stats_.skip_prim;
      stats_.pass_last_skip[g_current_pass & 31] = "prim";
      return false;
  }
  DXGI_FORMAT rtv_formats[4];
  uint32_t rt_count = 0;
  DXGI_FORMAT dsv_format = DXGI_FORMAT_UNKNOWN;
  if (!BindRenderTargets(base, dev, rtv_formats, rt_count, dsv_format)) {
    ++stats_.skip_rt;
    stats_.pass_last_skip[g_current_pass & 31] = "rt";
    return false;
  }
  // After binding targets: EDRAM reinterpretation inside BindRenderTargets
  // may have changed the root signature and heaps.
  if (!frame_state_bound_) {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
    command_list_->SetDescriptorHeaps(2, heaps);
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    for (uint32_t i = 0; i < 3; ++i) {
      command_list_->SetGraphicsRootDescriptorTable(3 + i,
                                                    srv_heap_->GetGPUDescriptorHandleForHeapStart());
    }
    command_list_->SetGraphicsRootDescriptorTable(
        6, sampler_heap_->GetGPUDescriptorHandleForHeapStart());
    frame_state_bound_ = true;
  }
  // Primitive restart only matters for strips.
  if (topology != D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP &&
      topology != D3D_PRIMITIVE_TOPOLOGY_LINESTRIP) {
    strip_cut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
  }
  ID3D12PipelineState* pso = GetPipeline(base, dev, vs_hash, ps_hash, decl, topology_type,
                                         rtv_formats, rt_count, dsv_format, strip_cut);
  if (!pso) {
    ++stats_.skip_pso;
    stats_.pass_last_skip[g_current_pass & 31] = "pso";
    return false;
  }
  // Constants may create/upload textures (copy commands): record them before
  // the draw, the render targets stay bound.
  if (trace_state_ == 1 && ps_hash == 0x1EFDCC2FE2C551A4ull) {
    static int logged = 0;
    if (logged++ < 3) {
      std::string m;
      for (uint32_t r : {162u, 164u, 165u, 175u, 177u, 178u, 189u, 190u, 24u, 25u, 26u, 27u, 28u, 29u, 30u, 31u, 32u, 33u, 64u, 65u, 66u}) {
        uint32_t a = dev + kDevPsConstants + r * 16;
        m += fmt::format(" c{}=({:.4g},{:.4g},{:.4g},{:.4g})", r, LoadF32(base, a), LoadF32(base, a + 4),
                         LoadF32(base, a + 8), LoadF32(base, a + 12));
      }
      REXLOG_INFO("trace ground ps consts (pre-flush, pending ring {}):{} psloop0 {:08X} psbools {:08X} {:08X}",
                  pending_ring_constants_.size(), m, Load32(base, dev + kDevPsLoops),
                  Load32(base, dev + kDevPsBools), Load32(base, dev + kDevPsBools + 4));
      uint32_t f[6];
      for (uint32_t d = 0; d < 6; ++d) f[d] = Load32(base, dev + kDevFetchConstants + 24 * 12 + 4 * d);
      DecodedTexture t;
      const char* why = nullptr;
      std::string texels;
      if (DecodeTexture(f, t, &why)) {
        // RGBA16F rows: sample 9 points along each row.
        for (uint32_t row = 0; row < t.height && t.format == DXGI_FORMAT_R16G16B16A16_FLOAT; ++row) {
          for (uint32_t x : {0u, 64u, 128u, 256u, 512u, 768u, 1024u, 1536u, 2047u}) {
            const uint16_t* px = reinterpret_cast<const uint16_t*>(
                t.data.data() + t.levels[0].offset + row * t.levels[0].row_pitch + x * 8);
            auto h = [](uint16_t v) {
              uint32_t sign = v >> 15, e = (v >> 10) & 31, m = v & 1023;
              float f = e ? std::ldexp(float(m | 1024), int(e) - 25) : std::ldexp(float(m), -24);
              return sign ? -f : f;
            };
            texels += fmt::format(" r{}x{}=({:.3f},{:.3f},{:.3f},{:.3f})", row, x, h(px[0]), h(px[1]),
                                  h(px[2]), h(px[3]));
          }
        }
      }
      REXLOG_INFO("trace fog table fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}: {}x{} dxgi {} map {:X} data{}",
                  f[0], f[1], f[2], f[3], f[4], f[5], t.width, t.height, int(t.format), t.component_mapping, texels);
    }
  }
  current_ps_hash_ = ps_hash;
  current_vs_hash_ = vs_hash;
  if (!UploadConstants(base, dev)) {
    return false;
  }
  if ((ssao_ || soft_particles_) && !camera_valid_ && g_current_pass == kPassRenderOpaque) {
    CaptureCamera();
  }
  ApplyFixedFunctionState(base, dev);
  command_list_->SetPipelineState(pso);
  command_list_->IASetPrimitiveTopology(topology);
  return true;
}

void Renderer::SwapBufferRange(uint8_t* base, const BufferEntry& entry, uint32_t begin,
                               uint32_t end, uint8_t* data) {
  uint32_t size = end - begin;
  const uint8_t* src = GuestPtr(base, 0xA0000000u + entry.address + begin, size);
  std::memcpy(data, src, size);
  if (entry.index_format == 1) {
    auto* p = reinterpret_cast<uint16_t*>(data);
    for (uint32_t i = 0; i < size / 2; ++i) p[i] = __builtin_bswap16(p[i]);
    return;
  }
  if (entry.index_format == 2) {
    auto* p = reinterpret_cast<uint32_t*>(data);
    for (uint32_t i = 0; i < size / 4; ++i) p[i] = __builtin_bswap32(p[i]);
    return;
  }
  // Vertex data: swap each element of the declaration for this stream. `begin`
  // is aligned to a vertex (phase + k * stride).
  uint32_t decl = entry.decl, stride = entry.stride;
  uint32_t decl_count = Load32(base, decl + 0x18);
  uint32_t stream = entry.index_format >> 8;
  for (uint32_t i = 0; i < decl_count; ++i) {
    uint32_t e = decl + 0x34 + 12 * i;
    uint32_t e_stream = Load16(base, e);
    if (e_stream == 0xFF) break;
    if (e_stream != stream) continue;
    uint32_t offset = Load16(base, e + 2);
    DeclFormat fmt = MapDeclType(Load32(base, e + 4));
    if (!fmt.size || offset + fmt.size > stride) continue;
    for (uint32_t v = 0; v + stride <= size; v += stride) {
      uint8_t* p = data + v + offset;
      if (fmt.swap == 4) {
        for (uint32_t w = 0; w < fmt.size; w += 4) {
          uint32_t x;
          std::memcpy(&x, p + w, 4);
          x = __builtin_bswap32(x);
          std::memcpy(p + w, &x, 4);
        }
      } else {
        for (uint32_t w = 0; w < fmt.size; w += 2) {
          uint16_t x;
          std::memcpy(&x, p + w, 2);
          x = __builtin_bswap16(x);
          std::memcpy(p + w, &x, 2);
        }
      }
    }
  }
}

void Renderer::VertexRange::Resolve() {
  if (resolved) return;
  resolved = true;
  uint32_t isize = index32 ? 4 : 2;
  if (!ib_phys || uint64_t(start_index + index_count) * isize > ib_size) return;
  const uint8_t* idx = REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(ib_phys);
  uint32_t reset = index32 ? 0xFFFFFFFFu : 0xFFFFu;
  uint32_t lo = ~0u, hi = 0;
  // SSE4.1: big-endian indices byte-swapped with pshufb; reset indices are
  // excluded from the maximum (they can only be the minimum if every index is
  // a reset, which the lo > hi check below rejects).
  uint32_t i = start_index;
  const uint32_t end_index = start_index + index_count;
  const __m128i ones = _mm_set1_epi32(-1);
  if (!index32) {
    const __m128i swap = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
    __m128i vmin = ones, vmax = _mm_setzero_si128();
    for (; i + 8 <= end_index; i += 8) {
      __m128i v = _mm_shuffle_epi8(
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(idx + 2 * size_t(i))), swap);
      vmin = _mm_min_epu16(vmin, v);
      vmax = _mm_max_epu16(vmax, _mm_andnot_si128(_mm_cmpeq_epi16(v, ones), v));
    }
    lo = uint32_t(_mm_cvtsi128_si32(_mm_minpos_epu16(vmin))) & 0xFFFF;
    hi = ~uint32_t(_mm_cvtsi128_si32(_mm_minpos_epu16(_mm_xor_si128(vmax, ones)))) & 0xFFFF;
    if (lo == 0xFFFF) lo = ~0u;
  } else {
    const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    __m128i vmin = ones, vmax = _mm_setzero_si128();
    for (; i + 4 <= end_index; i += 4) {
      __m128i v = _mm_shuffle_epi8(
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(idx + 4 * size_t(i))), swap);
      vmin = _mm_min_epu32(vmin, v);
      vmax = _mm_max_epu32(vmax, _mm_andnot_si128(_mm_cmpeq_epi32(v, ones), v));
    }
    vmin = _mm_min_epu32(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(1, 0, 3, 2)));
    vmin = _mm_min_epu32(vmin, _mm_shuffle_epi32(vmin, _MM_SHUFFLE(2, 3, 0, 1)));
    vmax = _mm_max_epu32(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(1, 0, 3, 2)));
    vmax = _mm_max_epu32(vmax, _mm_shuffle_epi32(vmax, _MM_SHUFFLE(2, 3, 0, 1)));
    lo = uint32_t(_mm_cvtsi128_si32(vmin));
    hi = uint32_t(_mm_cvtsi128_si32(vmax));
  }
  for (; i < end_index; ++i) {
    uint32_t x;
    if (index32) {
      std::memcpy(&x, idx + 4 * size_t(i), 4);
      x = __builtin_bswap32(x);
    } else {
      uint16_t h;
      std::memcpy(&h, idx + 2 * size_t(i), 2);
      x = __builtin_bswap16(h);
    }
    if (x == reset) continue;
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  if (lo > hi) return;
  int64_t f = int64_t(lo) + base_vertex, e = int64_t(hi) + base_vertex + 1;
  if (f >= 0 && e > f) {
    first = uint32_t(f);
    end = uint32_t(e);
  }
}

// Worker: executes a guest-side buffer plan. Planned bytes were captured with
// the command, so SwapBufferRange reads them instead of live guest memory.
const Renderer::BufferEntry* Renderer::ApplyBuffer(uint8_t* base, const BufferPlan& plan) {
  thread_local std::vector<uint8_t> scratch;
  auto it = buffers_.find(plan.key);
  if (it != buffers_.end() && plan.action != 2) {
    BufferEntry& entry = it->second;
    if (plan.action == 1 && plan.end > plan.begin) {
      uint32_t b = plan.begin, e = plan.end;
      UploadAlloc upload;
      if (!Upload(e - b, 16, upload)) return nullptr;
      scratch.resize(e - b);
      SwapBufferRange(base, entry, b, e, scratch.data());
      std::memcpy(upload.cpu, scratch.data(), e - b);
      Transition(entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
      command_list_->CopyBufferRegion(entry.resource.Get(), b, upload.resource, upload.offset,
                                      e - b);
      Transition(entry.resource.Get(), entry.state,
                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
                     D3D12_RESOURCE_STATE_INDEX_BUFFER);
      ++stats_.buffer_partial_uploads;
      stats_.buffer_upload_bytes += e - b;
    }
    return &entry;
  }
  // First use (or unknown on this side): the whole buffer.
  uint32_t address = plan.address, size = plan.size;
  if (!size || size > (64u << 20)) {
    return nullptr;
  }
  UploadAlloc upload;
  if (!Upload(size, 16, upload)) {
    return nullptr;
  }
  ++stats_.buffer_uploads;
  stats_.buffer_upload_bytes += size;
  BufferEntry& entry = buffers_[plan.key];
  bool reuse = entry.resource && entry.size == size;
  entry.address = address;
  entry.size = size;
  entry.decl = plan.decl;
  entry.stride = plan.stride;
  entry.index_format = plan.format;
  entry.phase = plan.phase;
  uint32_t index_format = plan.format, stride = plan.stride, phase = plan.phase;
  // Byte-swap in cached CPU memory: the upload heap is write-combined, and
  // reading it back (as an in-place swap does) is extremely slow.
  scratch.resize(size);
  if (index_format == 1 || index_format == 2) {
    SwapBufferRange(base, entry, 0, size, scratch.data());
  } else {
    // Bytes outside whole vertices (before `phase`, trailing partial vertex)
    // stay unswapped: never read as vertices.
    std::memcpy(scratch.data(), GuestPtr(base, 0xA0000000u + address, size), size);
    uint32_t end = stride && size >= phase ? size - ((size - phase) % stride) : phase;
    if (end > phase) SwapBufferRange(base, entry, phase, end, scratch.data() + phase);
  }
  std::memcpy(upload.cpu, scratch.data(), size);
  if (!reuse) {
    if (entry.resource) Retire(entry.resource);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (size + 15) & ~15u;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    entry.state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, entry.state,
                                                nullptr, IID_PPV_ARGS(&entry.resource)))) {
      buffers_.erase(plan.key);
      return nullptr;
    }
  } else {
    Transition(entry.resource.Get(), entry.state, D3D12_RESOURCE_STATE_COPY_DEST);
  }
  command_list_->CopyBufferRegion(entry.resource.Get(), 0, upload.resource, upload.offset, size);
  Transition(entry.resource.Get(), entry.state,
             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER);
  return &entry;
}

// Guest side: decides what a draw must upload of a guest buffer (tracking the
// XDK Unlock invalidations) and captures those bytes with the command.
Renderer::BufferPlan Renderer::PlanBuffer(uint8_t* base, uint32_t address, uint32_t size,
                                          uint32_t decl, uint32_t stride, uint32_t index_format,
                                          uint32_t phase, uint32_t need_begin, uint32_t need_end,
                                          bool& ok) {
  BufferPlan plan;
  ok = size && size <= (64u << 20);
  if (!ok) return plan;
  uint64_t key_parts[] = {address, size, decl, stride, index_format, phase};
  plan.key = XXH3_64bits(key_parts, sizeof(key_parts));
  plan.address = address;
  plan.size = size;
  plan.decl = decl;
  plan.stride = stride;
  plan.format = index_format;
  plan.phase = phase;
  // Vertex data is swapped per vertex: the swap range must start on a vertex.
  auto align_range = [&](uint32_t& b, uint32_t& e) {
    e = std::min(e, size);
    if (index_format == 1) {
      b &= ~1u;
      e = (e + 1) & ~1u;
    } else if (index_format == 2) {
      b &= ~3u;
      e = (e + 3) & ~3u;
    } else if (stride) {
      b = b < phase ? phase : b - ((b - phase) % stride);
      uint32_t n = (e > b ? e - b + stride - 1 : 0) / stride;
      e = std::min(b + n * stride, size - ((size - phase) % stride));
    }
    e = std::min(e, size);
  };
  auto it = tracked_.find(plan.key);
  if (it != tracked_.end()) {
    TrackedBuffer& t = it->second;
    if (REXCVAR_GET(native_debug_buffers_always_dirty)) {
      t.dirty = true;
      t.clean.clear();
    }
    if (!t.dirty) return plan;
    uint32_t b = need_begin, e = need_end;
    align_range(b, e);
    if (e <= b) return plan;
    for (const auto& [cb, ce] : t.clean) {
      if (cb <= b && e <= ce) return plan;
    }
    plan.action = 1;
    plan.begin = b;
    plan.end = e;
    CaptureBytes(base, 0xA0000000u + address + b, e - b);
    t.clean.emplace_back(b, e);
    if (t.clean.size() > 64) {
      // Too fragmented: start over (later draws upload their ranges again).
      t.clean.clear();
      t.dirty = true;
    }
    return plan;
  }
  TrackedBuffer& t = tracked_[plan.key];
  t.address = address;
  t.size = size;
  for (uint32_t page = (address & 0x1FFFFFFF) >> 16;
       page <= ((address & 0x1FFFFFFF) + size - 1) >> 16; ++page) {
    buffer_pages_[page].push_back(&t);
  }
  plan.action = 2;
  plan.begin = 0;
  plan.end = size;
  CaptureBytes(base, 0xA0000000u + address, size);
  return plan;
}

void Renderer::InvalidateGuestRange(uint32_t address, uint32_t size) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (!size) {
    return;
  }
  // Physical addresses of buffers are compared on the low 29 bits.
  uint32_t a = address & 0x1FFFFFFF;
  uint64_t stamp = ++invalidation_stamp_;
  for (uint32_t page = a >> 16; page <= (a + size - 1) >> 16; ++page) {
    auto pit = buffer_pages_.find(page);
    if (pit == buffer_pages_.end()) continue;
    for (TrackedBuffer* tp : pit->second) {
      TrackedBuffer& t = *tp;
      if (t.invalidation_stamp == stamp) continue;  // seen on an earlier page
      t.invalidation_stamp = stamp;
      uint32_t b = t.address & 0x1FFFFFFF;
      if (t.dirty && t.clean.empty()) continue;  // already fully dirty
      if (b < a + size && a < b + t.size) {
        front_invalidations_.fetch_add(1, std::memory_order_relaxed);
        front_invalidated_bytes_.fetch_add(t.size, std::memory_order_relaxed);
        t.dirty = true;
        t.clean.clear();
      }
    }
  }
}

// Guest side: the vertex streams of the current draw (guest state at draw time)
// and their buffer plans.
bool Renderer::PlanStreams(uint8_t* base, uint32_t dev, uint32_t decl, VertexRange* range) {
  if (!decl) return false;
  uint32_t decl_count = std::min(Load32(base, decl + 0x18), 64u);
  uint32_t streams_used = 0;
  for (uint32_t i = 0; i < decl_count; ++i) {
    uint32_t e = decl + 0x34 + 12 * i;
    uint32_t s = Load16(base, e);
    if (s == 0xFF) break;
    if (s < 16) streams_used |= 1u << s;
  }
  for (uint32_t s = 0; s < 16; ++s) {
    if (!(streams_used & (1u << s))) continue;
    // Vertex fetch constant for stream s (XDK: slot 95 - s).
    uint32_t fetch0 = Load32(base, dev + 0x778 - 8 * s);
    uint32_t fetch1 = Load32(base, dev + 0x778 - 8 * s + 4);
    uint32_t address = fetch0 & ~3u;
    uint32_t size = ((fetch1 >> 2) & 0xFFFFFF) * 4;
    uint32_t stride = Load8(base, dev + 0x30E8 + s) * 4;
    if (!address || !size || !stride) {
      return false;
    }
    // Cache the whole guest vertex buffer (the stream offset of SetStreamSource
    // varies per draw into large shared buffers) and bind at an offset.
    uint32_t buffer_base = address, buffer_size = size;
    if (uint32_t vb_object = Load32(base, dev + 0x30A4 + 4 * s)) {
      uint32_t v = Load32(base, vb_object + 0x18) & ~3u;
      uint32_t phys = (v & 0x1FFFFFFFu) + (v >= 0xE0000000u ? 0x1000u : 0u);
      uint32_t full = ((Load32(base, vb_object + 0x1C) >> 2) & 0xFFFFFF) * 4;
      if (phys <= address && address + size <= phys + full) {
        buffer_base = phys;
        buffer_size = full;
      }
    }
    uint32_t offset = address - buffer_base;
    uint32_t phase = offset % stride;
    StreamPlan sp;
    sp.stream = s;
    sp.offset = offset;
    sp.size = size;
    sp.stride = stride;
    FrontStreamCache& sc = front_stream_cache_[s];
    if (!REXCVAR_GET(native_debug_buffers_always_dirty) && sc.tracked && sc.address == buffer_base && sc.size == buffer_size && sc.decl == decl &&
        sc.stride == stride && sc.phase == phase && !sc.tracked->dirty) {
      // Clean and cached: no lookup, no range needed.
      sp.buffer.key = sc.key;
      sp.buffer.address = buffer_base;
      sp.buffer.size = buffer_size;
      sp.buffer.decl = decl;
      sp.buffer.stride = stride;
      sp.buffer.format = s << 8;
      sp.buffer.phase = phase;
    } else {
      uint32_t need_begin = 0, need_end = ~0u;
      if (range) {
        range->Resolve();
        if (range->end != ~0u) {
          uint64_t b = uint64_t(offset) + uint64_t(range->first) * stride;
          uint64_t e = uint64_t(offset) + uint64_t(range->end) * stride;
          need_begin = uint32_t(std::min<uint64_t>(b, buffer_size));
          need_end = uint32_t(std::min<uint64_t>(e, buffer_size));
        }
      }
      bool ok = false;
      sp.buffer = PlanBuffer(base, buffer_base, buffer_size, decl, stride, s << 8, phase,
                             need_begin, need_end, ok);
      if (!ok) return false;
      auto it = tracked_.find(sp.buffer.key);
      sc.address = buffer_base;
      sc.size = buffer_size;
      sc.decl = decl;
      sc.stride = stride;
      sc.format = s << 8;
      sc.phase = phase;
      sc.key = sp.buffer.key;
      sc.tracked = it != tracked_.end() ? &it->second : nullptr;
    }
    if (REXCVAR_GET(native_debug_vb_addr) && address == uint32_t(REXCVAR_GET(native_debug_vb_addr))) {
      static int logged = 0;
      if (logged++ < 40) {
        std::string v;
        for (uint32_t k = 0; k < 12 && k * 4 < buffer_size; ++k)
          v += fmt::format(" {:.4f}", LoadF32(base, 0xA0000000u + buffer_base + offset + 4 * k));
        REXLOG_INFO("native dbg vb {:08X}+{} size {} stride {} plan action {} dirty {}:{}",
                    buffer_base, offset, buffer_size, stride, int(sp.buffer.action),
                    sc.tracked ? sc.tracked->dirty : -1, v);
      }
    }
    batch_->streams.push_back(sp);
  }
  return true;
}

// Worker: uploads (as planned) and binds the draw's vertex streams. Runs for
// every planned draw, even one skipped later, so host buffers stay in sync
// with the guest-side tracking.
bool Renderer::BindVertexStreams(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd) {
  bool ok = cmd.streams_ok;
  for (uint32_t i = 0; i < cmd.stream_count; ++i) {
    const StreamPlan& sp = batch.streams[cmd.stream_first + i];
    const BufferEntry* vb = ApplyBuffer(base, sp.buffer);
    if (!vb) {
      ok = false;
      continue;
    }
    D3D12_VERTEX_BUFFER_VIEW view{vb->resource->GetGPUVirtualAddress() + sp.offset, sp.size,
                                  sp.stride};
    command_list_->IASetVertexBuffers(sp.stream, 1, &view);
  }
  if (!ok) ++stats_.skip_vb;
  return ok;
}

void Renderer::ExecDrawVertices(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd) {
  uint32_t prim = cmd.u[0], start_vertex = cmd.u[1], vertex_count = cmd.u[2];
  crumb_prim_ = prim;
  crumb_count_ = vertex_count;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!BeginFrame()) return;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  D3D12_PRIMITIVE_TOPOLOGY topology;
  bool quads;
  bool streams_ok = BindVertexStreams(base, batch, cmd);
  if (!dev || !vertex_count || !PrepareDraw(base, dev, prim, topology, quads) || !streams_ok) {
    ++stats_.draws_skipped; ++stats_.pass_skips[g_current_pass & 31];
    return;
  }
  if (quads) {
    uint32_t quad_count = vertex_count / 4;
    UploadAlloc ib;
    if (!Upload(size_t(quad_count) * 6 * 4, 4, ib)) return;
    uint32_t* idx = reinterpret_cast<uint32_t*>(ib.cpu);
    for (uint32_t q = 0; q < quad_count; ++q) {
      uint32_t b = start_vertex + q * 4;
      idx[q * 6 + 0] = b;
      idx[q * 6 + 1] = b + 1;
      idx[q * 6 + 2] = b + 2;
      idx[q * 6 + 3] = b;
      idx[q * 6 + 4] = b + 2;
      idx[q * 6 + 5] = b + 3;
    }
    D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, quad_count * 24, DXGI_FORMAT_R32_UINT};
    command_list_->IASetIndexBuffer(&ibv);
    command_list_->DrawIndexedInstanced(quad_count * 6, 1, 0, 0, 0);
  } else {
    command_list_->DrawInstanced(vertex_count, 1, start_vertex, 0);
  }
  ++stats_.draws; ++trace_draws_; Breadcrumb();
  ++stats_.pass_draws[g_current_pass & 31];
}

void Renderer::ExecDrawIndexedVertices(uint8_t* base, const WorkBatch& batch,
                                       const WorkCmd& cmd) {
  uint32_t prim = cmd.u[0], start_index = cmd.u[2], index_count = cmd.u[3];
  int32_t base_vertex = int32_t(cmd.u[1]);
  crumb_prim_ = prim | 0x100;
  crumb_count_ = index_count;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!BeginFrame()) return;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  D3D12_PRIMITIVE_TOPOLOGY topology;
  bool quads;
  // Xenos strips restart at the all-ones index (the XDK enables reset for
  // indexed strips; D3D12 needs the cut value baked into the PSO).
  D3D12_INDEX_BUFFER_STRIP_CUT_VALUE cut = cmd.index32
                                               ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
                                               : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF;
  // Buffers first (planned uploads must happen even for skipped draws).
  bool streams_ok = BindVertexStreams(base, batch, cmd);
  const BufferEntry* ib = cmd.has_index ? ApplyBuffer(base, cmd.index) : nullptr;
  if (!dev || !index_count || !PrepareDraw(base, dev, prim, topology, quads, cut) || quads ||
      !streams_ok || !ib) {
    ++stats_.draws_skipped; ++stats_.pass_skips[g_current_pass & 31];
    return;
  }
  D3D12_INDEX_BUFFER_VIEW ibv{ib->resource->GetGPUVirtualAddress(), cmd.index_size,
                              cmd.index32 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT};
  command_list_->IASetIndexBuffer(&ibv);
  command_list_->DrawIndexedInstanced(index_count, 1, start_index, base_vertex, 0);
  ++stats_.draws; ++trace_draws_; Breadcrumb();
  ++stats_.pass_draws[g_current_pass & 31];
}

// Xenos rectangle list: every 3 vertices describe a rectangle. As in Xenia's
// rectangle list geometry shader, the longest edge (by position x/y) is the
// diagonal and the 4th corner mirrors the remaining vertex across it
// (v3 = d1 + d2 - corner, for all float attributes; parallelogram completion
// is affine-invariant, so doing it before the vertex shader is exact). Expands
// already byte-swapped vertices into a triangle list (6 vertices per rectangle).
static std::vector<uint8_t> ExpandRectList(uint8_t* base, uint32_t decl, const uint8_t* vertices,
                                           uint32_t vertex_count, uint32_t stride) {
  uint32_t rects = vertex_count / 3;
  std::vector<uint8_t> out(size_t(rects) * 6 * stride);
  uint32_t decl_count = Load32(base, decl + 0x18);
  struct Element {
    uint32_t offset, size;
  };
  std::vector<Element> floats;
  int32_t position = -1;  // offset of the position element (x, y floats)
  for (uint32_t i = 0; i < decl_count; ++i) {
    uint32_t e = decl + 0x34 + 12 * i;
    if (Load16(base, e) == 0xFF) break;
    uint32_t offset = Load16(base, e + 2);
    DeclFormat fmt = MapDeclType(Load32(base, e + 4));
    bool is_float = fmt.format == DXGI_FORMAT_R32_FLOAT || fmt.format == DXGI_FORMAT_R32G32_FLOAT ||
                    fmt.format == DXGI_FORMAT_R32G32B32_FLOAT ||
                    fmt.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (!is_float || offset + fmt.size > stride) continue;
    floats.push_back({offset, fmt.size});
    // Usage 0 = POSITION; otherwise the first float element with x and y.
    if (fmt.size >= 8 && (position < 0 || Load8(base, e + 9) == 0)) {
      if (position < 0 || Load8(base, e + 9) == 0) position = int32_t(offset);
    }
  }
  for (uint32_t r = 0; r < rects; ++r) {
    const uint8_t* v[3];
    for (int k = 0; k < 3; ++k) v[k] = vertices + (size_t(r) * 3 + k) * stride;
    // Longest edge = diagonal; `corner` is the vertex opposite to it.
    int corner = 0;
    if (position >= 0) {
      float p[3][2];
      for (int k = 0; k < 3; ++k) std::memcpy(p[k], v[k] + position, 8);
      auto len2 = [&](int i, int j) {
        float dx = p[i][0] - p[j][0], dy = p[i][1] - p[j][1];
        return dx * dx + dy * dy;
      };
      float e12 = len2(1, 2), e20 = len2(2, 0), e01 = len2(0, 1);
      if (e12 > e20 && e12 > e01) {
        corner = 0;
      } else {
        corner = e20 > e01 ? 1 : 2;
      }
    }
    const uint8_t* c = v[corner];
    const uint8_t* d1 = v[(corner + 1) % 3];
    const uint8_t* d2 = v[(corner + 2) % 3];
    std::vector<uint8_t> v3(c, c + stride);
    for (const Element& el : floats) {
      for (uint32_t k = 0; k < el.size; k += 4) {
        float a, b, o;
        std::memcpy(&a, d1 + el.offset + k, 4);
        std::memcpy(&b, d2 + el.offset + k, 4);
        std::memcpy(&o, c + el.offset + k, 4);
        float d = a + b - o;
        std::memcpy(v3.data() + el.offset + k, &d, 4);
      }
    }
    // Strip (corner, d1, d2, v3) as a list.
    uint8_t* dst = out.data() + size_t(r) * 6 * stride;
    const uint8_t* order[6] = {c, d1, d2, d2, d1, v3.data()};
    for (int k = 0; k < 6; ++k) std::memcpy(dst + size_t(k) * stride, order[k], stride);
  }
  return out;
}

void Renderer::ExecDrawInlineVertices(uint8_t* base, uint32_t prim, uint32_t data,
                                      uint32_t vertex_count, uint32_t stride) {
  crumb_prim_ = prim | 0x200;
  crumb_count_ = vertex_count;
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!BeginFrame()) return;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  D3D12_PRIMITIVE_TOPOLOGY topology;
  bool quads;
  if (!dev || !vertex_count || !stride || !data ||
      !PrepareDraw(base, dev, prim, topology, quads)) {
    ++stats_.draws_skipped; ++stats_.pass_skips[g_current_pass & 31];
    return;
  }
  uint32_t decl = Load32(base, dev + kDevVertexDecl);
  UploadAlloc vb;
  size_t vb_size = size_t(vertex_count) * stride;
  if (!Upload(vb_size, 16, vb)) {
    ++stats_.draws_skipped; ++stats_.pass_skips[g_current_pass & 31];
    return;
  }
  thread_local std::vector<uint8_t> inline_scratch;
  inline_scratch.resize(vb_size);
  uint8_t* vdata = inline_scratch.data();
  std::memcpy(vdata, GuestPtr(base, data, uint32_t(vb_size)), vb_size);
  uint32_t decl_count = Load32(base, decl + 0x18);
  for (uint32_t i = 0; i < decl_count; ++i) {
    uint32_t e = decl + 0x34 + 12 * i;
    if (Load16(base, e) == 0xFF) break;
    uint32_t offset = Load16(base, e + 2);
    DeclFormat fmt = MapDeclType(Load32(base, e + 4));
    if (!fmt.size || offset + fmt.size > stride) continue;
    for (uint32_t v = 0; v < vertex_count; ++v) {
      uint8_t* p = vdata + size_t(v) * stride + offset;
      if (fmt.swap == 4) {
        for (uint32_t w = 0; w < fmt.size; w += 4) {
          uint32_t x;
          std::memcpy(&x, p + w, 4);
          x = __builtin_bswap32(x);
          std::memcpy(p + w, &x, 4);
        }
      } else {
        for (uint32_t w = 0; w < fmt.size; w += 2) {
          uint16_t x;
          std::memcpy(&x, p + w, 2);
          x = __builtin_bswap16(x);
          std::memcpy(p + w, &x, 2);
        }
      }
    }
  }
  if (trace_state_ == 1) {
    std::string vs;
    for (uint32_t k = 0; k < std::min<uint32_t>(vertex_count * stride / 4, 40); ++k) {
      uint32_t u;
      std::memcpy(&u, vdata + 4 * k, 4);
      vs += fmt::format(" {:08X}", u);
    }
    REXLOG_INFO("trace   inline prim {} count {} stride {}:{}", prim, vertex_count, stride, vs);
  }
  if (prim == uint32_t(xenos::PrimitiveType::kRectangleList)) {
    std::vector<uint8_t> expanded = ExpandRectList(base, decl, vdata, vertex_count, stride);
    UploadAlloc rect_vb;
    if (expanded.empty() || !Upload(expanded.size(), 16, rect_vb)) {
      ++stats_.draws_skipped;
      return;
    }
    std::memcpy(rect_vb.cpu, expanded.data(), expanded.size());
    D3D12_VERTEX_BUFFER_VIEW rect_view{rect_vb.gpu, uint32_t(expanded.size()), stride};
    command_list_->IASetVertexBuffers(0, 1, &rect_view);
    command_list_->DrawInstanced(uint32_t(expanded.size() / stride), 1, 0, 0);
    ++stats_.draws; ++trace_draws_; Breadcrumb();
    ++stats_.pass_draws[g_current_pass & 31];
    return;
  }
  std::memcpy(vb.cpu, vdata, vb_size);
  D3D12_VERTEX_BUFFER_VIEW vbv{vb.gpu, uint32_t(vb_size), stride};
  command_list_->IASetVertexBuffers(0, 1, &vbv);
  if (quads) {
    uint32_t quad_count = vertex_count / 4;
    UploadAlloc ib;
    if (!Upload(size_t(quad_count) * 6 * 2, 4, ib)) return;
    uint16_t* idx = reinterpret_cast<uint16_t*>(ib.cpu);
    for (uint32_t q = 0; q < quad_count; ++q) {
      uint16_t b = uint16_t(q * 4);
      idx[q * 6 + 0] = b;
      idx[q * 6 + 1] = b + 1;
      idx[q * 6 + 2] = b + 2;
      idx[q * 6 + 3] = b;
      idx[q * 6 + 4] = b + 2;
      idx[q * 6 + 5] = b + 3;
    }
    D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, quad_count * 12, DXGI_FORMAT_R16_UINT};
    command_list_->IASetIndexBuffer(&ibv);
    command_list_->DrawIndexedInstanced(quad_count * 6, 1, 0, 0, 0);
  } else {
    command_list_->DrawInstanced(vertex_count, 1, 0, 0);
  }
  ++stats_.draws; ++trace_draws_; Breadcrumb();
  ++stats_.pass_draws[g_current_pass & 31];
}

void Renderer::ExecBeginTiling(uint8_t* base, uint32_t count, uint32_t rects,
                               uint32_t clear_color, float clear_z, uint32_t clear_stencil) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!BeginFrame()) return;
  uint32_t w = 0, h = 0;
  for (uint32_t i = 0; i < count && rects; ++i) {
    w = std::max(w, Load32(base, rects + 16 * i + 8));
    h = std::max(h, Load32(base, rects + 16 * i + 12));
  }
  if (trace_state_ == 1)
    REXLOG_INFO("trace p{} after {} draws: BeginTiling {} tiles {}x{}", g_current_pass, trace_draws_,
                count, w, h);
  tiling_active_ = w && h;
  tiling_width_ = w;
  tiling_height_ = h;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  // Clear the (full-size) tiling surfaces as the XDK does per tile.
  HostSurface* rt = GetSurface(base, Load32(base, dev + kDevRenderTargets), false);
  HostSurface* ds = GetSurface(base, Load32(base, dev + kDevDepthStencil), true);
  if (rt && clear_color) {
    float color[4];
    for (uint32_t i = 0; i < 4; ++i) color[i] = LoadF32(base, clear_color + 4 * i);
    if (trace_state_ == 1)
      REXLOG_INFO("trace BeginTiling clear {} color ({}, {}, {}, {})", SurfaceName(rt), color[0],
                  color[1], color[2], color[3]);
    // The EDRAM clear also defines the contents of every surface aliasing
    // that EDRAM (e.g. the 8888 decal/leaf buffer sharing base 936 with the
    // HDR scene target): clear the host aliases too.
    for (auto& [key, other] : surfaces_) {
      if (other.depth || other.edram_base != rt->edram_base || other.width != rt->width ||
          other.height != rt->height || other.scale != rt->scale) {
        continue;
      }
      Transition(other.resource.Get(), other.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
      command_list_->ClearRenderTargetView(other.view, color, 0, nullptr);
    }
  }
  if (ds) {
    Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    command_list_->ClearDepthStencilView(ds->view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                         std::clamp(clear_z, 0.0f, 1.0f), uint8_t(clear_stencil),
                                         0, nullptr);
  }
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
}

void Renderer::ExecEndTiling() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (trace_state_ == 1) REXLOG_INFO("trace p{} after {} draws: EndTiling", g_current_pass, trace_draws_);
  tiling_active_ = false;
}

void Renderer::CopyDepthRegion(HostSurface& src, ResolveTexture& dst, uint32_t x1, uint32_t y1,
                               uint32_t x2, uint32_t y2, uint32_t dx, uint32_t dy) {
  if (!src.srv_index) {
    src.srv_index = AllocSrvIndex();
    if (!src.srv_index) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srv.ViewDimension =
        src.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(
        src.resource.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                        src.srv_index));
  }
  if (!src.stencil_srv_index) {
    src.stencil_srv_index = AllocSrvIndex();
    if (!src.stencil_srv_index) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    srv.ViewDimension =
        src.samples > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (src.samples <= 1) {
      srv.Texture2D.MipLevels = 1;
      srv.Texture2D.PlaneSlice = 1;
    }
    device_->CreateShaderResourceView(
        src.resource.Get(), &srv,
        provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                        src.stencil_srv_index));
  }
  Transition(src.resource.Get(), src.state,
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Transition(dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (dst.raw_resource) {
    Transition(dst.raw_resource.Get(), dst.raw_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
  command_list_->SetDescriptorHeaps(2, heaps);
  command_list_->SetGraphicsRootSignature(depth_copy_root_signature_.Get());
  int32_t offset[2] = {int32_t(x1) - int32_t(dx), int32_t(y1) - int32_t(dy)};
  command_list_->SetGraphicsRoot32BitConstants(0, 2, offset, 0);
  command_list_->SetGraphicsRootDescriptorTable(
      1, provider_->OffsetViewDescriptor(srv_heap_->GetGPUDescriptorHandleForHeapStart(),
                                         src.srv_index));
  command_list_->SetGraphicsRootDescriptorTable(
      2, provider_->OffsetViewDescriptor(srv_heap_->GetGPUDescriptorHandleForHeapStart(),
                                         src.stencil_srv_index));
  command_list_->SetPipelineState(src.samples > 1 ? depth_copy_ms_pipeline_.Get()
                                                  : depth_copy_pipeline_.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = {dst.rtv, dst.raw_rtv.ptr ? dst.raw_rtv : dst.rtv};
  command_list_->OMSetRenderTargets(dst.raw_resource ? 2 : 1, targets, FALSE, nullptr);
  D3D12_VIEWPORT vp{float(dx), float(dy), float(x2 - x1), float(y2 - y1), 0.0f, 1.0f};
  D3D12_RECT sc{LONG(dx), LONG(dy), LONG(dx + (x2 - x1)), LONG(dy + (y2 - y1))};
  command_list_->RSSetViewports(1, &vp);
  command_list_->RSSetScissorRects(1, &sc);
  command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list_->DrawInstanced(3, 1, 0, 0);
  Breadcrumb("depth copy");
  if (dst.raw_resource) {
    Transition(dst.raw_resource.Get(), dst.raw_state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  }
  // The guest state (root signature, heaps, targets) must be rebound.
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
}

void Renderer::ExecResolve(uint8_t* base, uint32_t flags, uint32_t src_rect,
                           uint32_t dest_texture, uint32_t dest_point, uint32_t clear_color,
                           float clear_z, uint32_t clear_stencil) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!BeginFrame()) return;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  uint32_t source = flags & 7;
  bool depth = source == 4;
  HostSurface* src = GetSurface(
      base, Load32(base, dev + (depth ? kDevDepthStencil : kDevRenderTargets + 4 * source)), depth);
  if (!src) return;
  ++stats_.resolves;
  for (SlotCache& sc : slot_cache_) sc.frame = ~0ull;
  // The command's PM4 words were scanned by Execute (copy draw included).
  uint32_t ring_before = ring_last_;
  uint64_t copies_before = exec_copies_before_;
  uint32_t copy_dest_info = mirror_.last_copy_dest_info;
  if (trace_state_ == 1) {
    {
      std::string dw;
      for (uint32_t a = ring_before; a < ring_last_ && a < ring_before + 4 * 96; a += 4)
        dw += fmt::format(" {:08X}", Load32(base, a));
      REXLOG_INFO("trace resolve packets:{}", dw);
    }
    REXLOG_INFO("trace resolve ring {:08X}->{:08X} copies {} dest_info {:08X} dest_base {:08X}",
                ring_before, ring_last_, mirror_.copy_draws - copies_before,
                mirror_.last_copy_dest_info, mirror_.last_copy_dest_base);
  }
  if (trace_state_ == 1) {
    uint32_t r[4] = {};
    if (src_rect) for (int i = 0; i < 4; ++i) r[i] = Load32(base, src_rect + 4 * i);
    uint32_t dp[2] = {};
    if (dest_point) for (int i = 0; i < 2; ++i) dp[i] = Load32(base, dest_point + 4 * i);
    uint32_t df1 = dest_texture ? Load32(base, dest_texture + 0x20) : 0;
    uint32_t df2 = dest_texture ? Load32(base, dest_texture + 0x24) : 0;
    REXLOG_INFO("trace p{} after {} draws: resolve flags {:X} src {} rect {},{}-{},{} -> {:08X} {}x{} fmt {} at {},{} (ptr {:08X}) destinfo {:08X}",
                g_current_pass, trace_draws_, flags, SurfaceName(src), r[0], r[1], r[2], r[3],
                dest_texture ? GuestPhysical(df1 & 0xFFFFF000u) : 0, (df2 & 0x1FFF) + 1,
                ((df2 >> 13) & 0x1FFF) + 1, df1 & 0x3F, dp[0], dp[1], dest_point, copy_dest_info);
    trace_draws_ = 0;
  }

  if (dest_texture) {
    uint32_t fetch[6];
    for (uint32_t d = 0; d < 6; ++d) fetch[d] = Load32(base, dest_texture + 0x1C + 4 * d);
    uint32_t dest_base = GuestPhysical(fetch[1] & 0xFFFFF000u);
    uint32_t dest_w = (fetch[2] & 0x1FFF) + 1;
    uint32_t dest_h = ((fetch[2] >> 13) & 0x1FFF) + 1;
    // Depth resolves become R32_FLOAT textures written by a shader copy (depth
    // formats only allow whole-subresource copies; the shadow atlas needs
    // region copies at a destination point).
    DXGI_FORMAT dst_format = depth ? DXGI_FORMAT_R32_FLOAT : src->resource_format;
    // The same guest memory is resolved at different sizes within a frame (e.g.
    // downsample chains): one host texture per size, never recreated per frame.
    uint64_t resolve_key = (uint64_t(dest_base) << 32) | (uint32_t(dst_format) & 0xFF) |
                           (((dest_w - 1) & 0x1FFF) << 8) | (((dest_h - 1) & 0x7FF) << 21);
    resolve_latest_[dest_base] = resolve_key;
    ResolveTexture& dst = resolve_textures_[resolve_key];
    std::memcpy(dst.guest_fetch, fetch, sizeof(fetch));
    if (!dst.resource || dst.width != dest_w || dst.height != dest_h ||
        dst.scale != src->scale) {
      Retire(dst.resource);
      for (auto& [mapping, index] : dst.srv_by_mapping) RetireSrvIndex(index);
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = dst.rtv, raw_rtv = dst.raw_rtv;
      Retire(dst.raw_resource);
      for (auto& [mapping, index] : dst.raw_srv_by_mapping) RetireSrvIndex(index);
      if (dst.plain_srv) RetireSrvIndex(dst.plain_srv);
      dst = ResolveTexture{};
      dst.rtv = rtv;
      dst.raw_rtv = raw_rtv;
      dst.width = dest_w;
      dst.height = dest_h;
      dst.scale = src->scale;
      dst.shadow = src->shadow;
      ++stats_.resolve_textures_created;
      dst.format = dst_format;
      dst.srv_format = depth ? DXGI_FORMAT_R32_FLOAT : src->srv_format;
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = HostPx(dest_w, dst.scale);
      desc.Height = HostPx(dest_h, dst.scale);
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.Format = dst.format;
      desc.SampleDesc.Count = 1;
      if (depth) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      D3D12_HEAP_PROPERTIES heap{};
      heap.Type = D3D12_HEAP_TYPE_DEFAULT;
      dst.state = D3D12_RESOURCE_STATE_COPY_DEST;
      if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, dst.state,
                                                  nullptr, IID_PPV_ARGS(&dst.resource)))) {
        resolve_textures_.erase(resolve_key);
        resolve_latest_.erase(dest_base);
        return;
      }
      if (depth) {
        if (!dst.rtv.ptr) {
          dst.rtv = provider_->OffsetRTVDescriptor(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                   rtv_heap_next_++ % kRtvHeapSize);
        }
        device_->CreateRenderTargetView(dst.resource.Get(), nullptr, dst.rtv);
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.raw_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        // The raw D24S8-as-RGBA8 copy (k_8_8_8_8 fetches of a depth resolve) is
        // never read from shadow maps; at shadow_quality 4 it would add 512 MB.
        if (!dst.shadow && SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       dst.raw_state, nullptr,
                                                       IID_PPV_ARGS(&dst.raw_resource)))) {
          if (!dst.raw_rtv.ptr) {
            dst.raw_rtv = provider_->OffsetRTVDescriptor(
                rtv_heap_->GetCPUDescriptorHandleForHeapStart(), rtv_heap_next_++ % kRtvHeapSize);
          }
          device_->CreateRenderTargetView(dst.raw_resource.Get(), nullptr, dst.raw_rtv);
        }
      }
    }
    std::memcpy(dst.guest_fetch, fetch, sizeof(fetch));
    // copy_dest_swap: the copy exchanges red and blue (A8R8G8B8 destinations);
    // applied as a component swap in the texture views.
    if (mirror_.copy_draws != copies_before && !depth) {
      bool swap = (copy_dest_info >> 24) & 1;
      if (swap != dst.swap_rb) {
        dst.swap_rb = swap;
        for (auto& [mapping, index] : dst.srv_by_mapping) RetireSrvIndex(index);
        dst.srv_by_mapping.clear();
      }
    }
    // Source rectangle (default: whole destination).
    uint32_t x1 = 0, y1 = 0, x2 = std::min(dest_w, src->width), y2 = std::min(dest_h, src->height);
    if (src_rect) {
      x1 = Load32(base, src_rect + 0);
      y1 = Load32(base, src_rect + 4);
      x2 = Load32(base, src_rect + 8);
      y2 = Load32(base, src_rect + 12);
    }
    uint32_t dx = x1, dy = y1;
    if (dest_point) {
      dx = Load32(base, dest_point + 0);
      dy = Load32(base, dest_point + 4);
      // Predicated tiling: per-tile resolves of a full-screen target that pass
      // a (0,0) destination keep the tile at its screen position.
      if (tiling_active_ && REXCVAR_GET(native_tile_resolve_in_place) && !dx && !dy &&
          (x1 || y1) && dest_w >= src->width && dest_h >= src->height) {
        dx = x1;
        dy = y1;
      }
    }
    x2 = std::min(x2, src->width);
    y2 = std::min(y2, src->height);
    if (dx + (x2 - x1) > dest_w) x2 = x1 + (dest_w - std::min(dest_w, dx));
    if (dy + (y2 - y1) > dest_h) y2 = y1 + (dest_h - std::min(dest_h, dy));
    if (x2 > x1 && y2 > y1 && REXCVAR_GET(native_debug_resolve_mode) != 1) {
      // Guest pixels -> host pixels.
      const float k = src->scale;
      x1 = HostPx(x1, k);
      y1 = HostPx(y1, k);
      x2 = HostPx(x2, k);
      y2 = HostPx(y2, k);
      dx = HostPx(dx, k);
      dy = HostPx(dy, k);
      if (!depth) {
        Transition(src->resource.Get(), src->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_COPY_DEST);
      }
      D3D12_TEXTURE_COPY_LOCATION s{}, d{};
      s.pResource = src->resource.Get();
      s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      d.pResource = dst.resource.Get();
      d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      D3D12_BOX box{x1, y1, 0, x2, y2, 1};
      if (depth) {
        CopyDepthRegion(*src, dst, x1, y1, x2, y2, dx, dy);
        // The scene depth (not a shadow map) for soft particles later in the frame.
        if (!dst.shadow && dest_w >= 512) scene_depth_key_ = resolve_key;
      } else if (src->samples > 1) {
        // Xenos color resolves average the samples.
        Transition(src->resource.Get(), src->state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
        Transition(dst.resource.Get(), dst.state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
        if (!resolve_list_) command_list_.As(&resolve_list_);
        D3D12_RECT rect{LONG(x1), LONG(y1), LONG(x2), LONG(y2)};
        resolve_list_->ResolveSubresourceRegion(dst.resource.Get(), 0, dx, dy,
                                                src->resource.Get(), 0, &rect,
                                                src->resource_format, D3D12_RESOLVE_MODE_AVERAGE);
        Breadcrumb("msaa resolve");
      } else {
        command_list_->CopyTextureRegion(&d, dx, dy, 0, &s, &box);
        Breadcrumb("color resolve");
      }
      Transition(dst.resource.Get(), dst.state,
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
  }

  // Clears requested with the resolve.
  if (REXCVAR_GET(native_debug_resolve_mode) == 2) flags &= ~0x300u;
  // Xenos clears only the resolved EDRAM rectangle: with predicated tiling
  // each tile's resolve clears that tile for the next pass, and our host
  // surfaces are untiled, so clearing everything would wipe the other tiles.
  D3D12_RECT clear_rect{0, 0, 16384, 16384};
  if (src_rect) {
    clear_rect = {LONG(Load32(base, src_rect + 0)), LONG(Load32(base, src_rect + 4)),
                  LONG(Load32(base, src_rect + 8)), LONG(Load32(base, src_rect + 12))};
  }
  if (flags & 0x100) {
    HostSurface* rt = GetSurface(base, Load32(base, dev + kDevRenderTargets), false);
    if (rt) {
      float color[4] = {};
      if (clear_color) {
        for (uint32_t i = 0; i < 4; ++i) color[i] = LoadF32(base, clear_color + 4 * i);
      }
      D3D12_RECT r = clear_rect;
      r.right = std::min<LONG>(r.right, LONG(rt->width));
      r.bottom = std::min<LONG>(r.bottom, LONG(rt->height));
      r = ScaleRect(r, rt->scale);
      // Host surfaces of other formats at the same EDRAM base alias the same
      // memory on Xenos (e.g. the 8888 and HDR scene targets at 936).
      for (auto& [key, other] : surfaces_) {
        if (other.depth || other.edram_base != rt->edram_base || other.width != rt->width ||
            other.height != rt->height || other.scale != rt->scale) {
          continue;
        }
        Transition(other.resource.Get(), other.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        command_list_->ClearRenderTargetView(other.view, color, 1, &r);
      }
    }
  }
  if (flags & 0x200) {
    HostSurface* ds = GetSurface(base, Load32(base, dev + kDevDepthStencil), true);
    if (ds) {
      D3D12_RECT r = clear_rect;
      r.right = std::min<LONG>(r.right, LONG(ds->width));
      r.bottom = std::min<LONG>(r.bottom, LONG(ds->height));
      r = ScaleRect(r, ds->scale);
      {
        static int logged = 0;
        if (logged < 200 && REXCVAR_GET(native_debug_clears)) {
          ++logged;
          REXLOG_INFO("native dbg depth clear {} z {} stencil {} rect {},{}-{},{} pass {} swap {}",
                      SurfaceName(ds), clear_z, clear_stencil, r.left, r.top, r.right, r.bottom,
                      g_current_pass, swap_number_);
        }
      }
      Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      command_list_->ClearDepthStencilView(ds->view,
                                           D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                           std::clamp(clear_z, 0.0f, 1.0f),
                                           uint8_t(clear_stencil), 1, &r);
    }
  }
  // Render target states changed; force a rebind at the next draw.
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
}

void Renderer::EndFrameAndPresent(uint32_t front_buffer_address) {
  // Blit the resolved front buffer into the output target.
  auto latest = resolve_latest_.find(front_buffer_address);
  auto it = latest != resolve_latest_.end() ? resolve_textures_.find(latest->second)
                                            : resolve_textures_.end();
  Transition(output_rt(), output_rt_states_[frame_index_], D3D12_RESOURCE_STATE_RENDER_TARGET);
  if (it != resolve_textures_.end()) {
    ResolveTexture& fb = it->second;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = fb.srv_format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    // One heap slot per frame slot for the blit source, allocated once. (Was:
    // allocate every frame and give back the heap top - wrong when the index
    // came from the retired list: the top index belonged to a live texture,
    // whose descriptor the next allocation then overwrote.)
    static uint32_t blit_slots[kFramesInFlight] = {};
    if (!blit_slots[frame_index_]) blit_slots[frame_index_] = AllocSrvIndex();
    uint32_t srv_index = blit_slots[frame_index_];
    auto cpu = provider_->OffsetViewDescriptor(srv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                               srv_index);
    device_->CreateShaderResourceView(fb.resource.Get(), &srv, cpu);
    Transition(fb.resource.Get(), fb.state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), sampler_heap_.Get()};
    command_list_->SetDescriptorHeaps(2, heaps);
    command_list_->SetGraphicsRootSignature(blit_root_signature_.Get());
    command_list_->SetGraphicsRootDescriptorTable(
        0, provider_->OffsetViewDescriptor(srv_heap_->GetGPUDescriptorHandleForHeapStart(),
                                           srv_index));
    // Guest display gamma ramp (Xenos applies it at scan-out; Xenia too).
    UploadAlloc gamma;
    if (Upload(1024 + 16, 256, gamma)) {
      uint32_t* g = reinterpret_cast<uint32_t*>(gamma.cpu);
      bool any = false;
      if (!graphics_ || !graphics_->GetGammaRamp256(g)) std::memset(g, 0, 1024);
      for (uint32_t i = 0; i < 256; ++i) any |= g[i] != 0;
      g[256] = (any && REXCVAR_GET(native_gamma_ramp)) ? 1 : 0;
      command_list_->SetGraphicsRootConstantBufferView(1, gamma.gpu);
    }
    command_list_->SetPipelineState(blit_pipeline_.Get());
    command_list_->OMSetRenderTargets(1, &output_rtvs_[frame_index_], FALSE, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(output_width_), float(output_height_), 0, 1};
    D3D12_RECT sc{0, 0, LONG(output_width_), LONG(output_height_)};
    command_list_->RSSetViewports(1, &vp);
    command_list_->RSSetScissorRects(1, &sc);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->DrawInstanced(3, 1, 0, 0);
    frame_state_bound_ = false;
  } else {
    float color[4] = {0.3f, 0.0f, 0.3f, 1.0f};  // magenta-ish: no front buffer resolved
    command_list_->ClearRenderTargetView(output_rtvs_[frame_index_], color, 0, nullptr);
  }

  // native_present=false: render (and dump) natively but leave presentation to
  // Xenos, for same-run A/B comparisons against its screenshots.
  if (!REXCVAR_GET(native_ab_mode) && !REXCVAR_GET(native_debug_no_present))
  presenter_->RefreshGuestOutput(
      output_width_, output_height_, 1280, 720,
      [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& d3d12_context =
            static_cast<rex::ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
        ID3D12Resource* output = d3d12_context.resource_uav_capable();
        Transition(output_rt(), output_rt_states_[frame_index_],
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_RESOURCE_STATES out_state =
            rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        if (!own_queue_) {
          Transition(output, out_state, D3D12_RESOURCE_STATE_COPY_DEST);
          command_list_->CopyResource(output, output_rt());
          Transition(output, out_state,
                     rex::ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
        }
        EndFrameTimestamp();
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        frame_open_ = false;
        if (own_queue_) {
          // The presenter's guest output belongs to the direct queue timeline:
          // copy there, after this frame's native work.
          queue_->Signal(fence_.Get(), next_fence_value_);
          ID3D12CommandQueue* direct = provider_->GetDirectQueue();
          direct->Wait(fence_.Get(), next_fence_value_);
          ++next_fence_value_;
          present_allocators_[frame_index_]->Reset();
          present_list_->Reset(present_allocators_[frame_index_].Get(), nullptr);
          D3D12_RESOURCE_BARRIER barrier{};
          barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
          barrier.Transition.pResource = output;
          barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
          barrier.Transition.StateBefore = out_state;
          barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
          if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
            present_list_->ResourceBarrier(1, &barrier);
          }
          present_list_->CopyResource(output, output_rt());
          std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
          if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
            present_list_->ResourceBarrier(1, &barrier);
          }
          present_list_->Close();
          ID3D12CommandList* present_lists[] = {present_list_.Get()};
          direct->ExecuteCommandLists(1, present_lists);
          direct->Signal(present_fence_.Get(), present_fence_next_);
          present_fence_values_[frame_index_] = present_fence_next_++;
        }
        context.SetIs8bpc(true);
        return true;
      });

  if (frame_open_) {
    EndFrameTimestamp();
    command_list_->Close();
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    frame_open_ = false;
  }
  queue_->Signal(fence_.Get(), next_fence_value_);
  frame_fence_values_[frame_index_] = next_fence_value_++;
  if (dump_all_active_) {
    dump_all_active_ = false;
    frame_dump_done_ = true;
    REXLOG_INFO("native: per-pass dump done");
  }
  bool dump_by_swap = REXCVAR_GET(native_dump_swap) > 0 &&
                      swap_number_ == uint64_t(REXCVAR_GET(native_dump_swap));
  if (!frame_dump_done_ && REXCVAR_GET(native_dump_after_pass) < 0 &&
      (dump_by_swap || (REXCVAR_GET(native_dump_frame_at_s) > 0 &&
                        rex::perf::BenchElapsedMs() >=
                            REXCVAR_GET(native_dump_frame_at_s) * 1000.0))) {
    frame_dump_done_ = true;
    DumpFrameResources();
  }
  // Multi-swap A/B (native_ab_swaps): the output only, prefixed by the swap.
  {
    static const std::vector<uint64_t> ab_swaps = [] {
      std::vector<uint64_t> v;
      std::string list = REXCVAR_GET(native_ab_swaps);
      size_t pos = 0;
      while (pos < list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        try {
          v.push_back(std::stoull(list.substr(pos, comma - pos)));
        } catch (...) {
        }
        pos = comma + 1;
      }
      return v;
    }();
    if (std::find(ab_swaps.begin(), ab_swaps.end(), swap_number_) != ab_swaps.end()) {
      DumpFrameResources(fmt::format("s{:06}_", swap_number_), false, true);
    }
  }
  std::memset(pass_draw_index_, 0, sizeof(pass_draw_index_));
  if (trace_state_ == 1) {
    REXLOG_INFO("trace end of frame ({} draws since last event)", trace_draws_);
    trace_state_ = 2;
  } else if (trace_state_ == 0 &&
             ((REXCVAR_GET(native_trace_swap) > 0 &&
               swap_number_ + 1 == uint64_t(REXCVAR_GET(native_trace_swap))) ||
              (REXCVAR_GET(native_trace_frame_at_s) > 0 &&
               rex::perf::BenchElapsedMs() >= REXCVAR_GET(native_trace_frame_at_s) * 1000.0))) {
    trace_state_ = 1;
    trace_draws_ = 0;
    REXLOG_INFO("trace begin frame {}", frame_count_ + 1);
  }
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
  ++frame_count_;
  static double last_buffer_log = 0;
  if (rex::perf::BenchElapsedMs() - last_buffer_log > 2000) {
    last_buffer_log = rex::perf::BenchElapsedMs();
    REXLOG_INFO("native: buffers uploads {} partial {} ({} MB) invalidations {} ({} MB) live {}",
                stats_.buffer_uploads, stats_.buffer_partial_uploads,
                stats_.buffer_upload_bytes >> 20, front_invalidations_.load(),
                front_invalidated_bytes_.load() >> 20, buffers_.size());
    std::string per_pass;
    for (int p = 0; p < 32; ++p) {
      if (stats_.pass_draws[p] || stats_.pass_skips[p]) {
        per_pass += fmt::format(" p{}:{}/{}{}", p, stats_.pass_draws[p], stats_.pass_skips[p],
                                stats_.pass_last_skip[p] ? stats_.pass_last_skip[p] : "");
      }
    }
    REXLOG_INFO("native: t={:.0f}s swap {} per pass drawn/skipped:{}",
                rex::perf::BenchElapsedMs() / 1000, swap_number_, per_pass);
    {
      std::string at;
      for (int p = 0; p < 32; ++p) {
        if (stats_.a2c_draws[p] || stats_.alpha_test_draws[p]) {
          at += fmt::format(" p{}:{}/{}", p, stats_.a2c_draws[p], stats_.alpha_test_draws[p]);
        }
      }
      if (!at.empty()) REXLOG_INFO("native: alpha test as coverage / hard per pass:{}", at);
    }
    {
      static Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
      if (!adapter) {
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
          factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
        }
      }
      DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
      if (adapter) {
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
      }
      REXLOG_INFO("native: vram {} MB / budget {} MB, sysmem {} MB | textures {} resolves {} "
                  "surfaces {} buffers {} retired {} srv_next {} free_srvs {} rtv_next {} upload_peak {} KB "
                  "overflow_pages {}",
                  local.CurrentUsage >> 20, local.Budget >> 20, nonlocal.CurrentUsage >> 20,
                  textures_.size(), resolve_textures_.size(), surfaces_.size(), buffers_.size(),
                  retired_.size(), srv_heap_next_, retired_srvs_.size(), rtv_heap_next_,
                  upload_peak_ >> 10, overflow_pages_created_);
    }
    REXLOG_INFO("native: gpu fence wait {} ms total, frames {}", stats_.gpu_wait_us / 1000,
                frame_count_);
    REXLOG_INFO("native: pm4 mirror packets {} dwords {} indirect {} ({} dwords) resyncs {}",
                mirror_.packets, mirror_.dwords, mirror_.indirect_buffers,
                mirror_.indirect_dwords, ring_resyncs_);
  }
  if ((frame_count_ % 600) == 0) {
    REXLOG_INFO(
        "native: frame {} draws={} skipped={} (shader {} pso {} prim {} rt {} vb {}) resolves={} "
        "(textures created {}) psos={} textures={} buffers={}",
        frame_count_, stats_.draws, stats_.draws_skipped, stats_.skip_shader, stats_.skip_pso,
        stats_.skip_prim, stats_.skip_rt, stats_.skip_vb, stats_.resolves,
        stats_.resolve_textures_created, stats_.pso_created,
        stats_.textures_created, buffers_.size());
    std::string per_pass;
    for (int p = 0; p < 32; ++p) {
      if (stats_.pass_draws[p] || stats_.pass_skips[p]) {
        per_pass += fmt::format(" p{}:{}/{}{}", p, stats_.pass_draws[p], stats_.pass_skips[p],
                                stats_.pass_last_skip[p] ? stats_.pass_last_skip[p] : "");
      }
    }
    REXLOG_INFO("native: per pass drawn/skipped:{}", per_pass);
  }
}

void Renderer::ExecOnPassEnd(int pass) {
  // native_dump_after_pass=99: dump the surfaces after every pass of one frame
  // (starting at the shadow pass), files prefixed with the pass number.
  bool all = REXCVAR_GET(native_dump_after_pass) == 99;
  if (all && !frame_dump_done_ && REXCVAR_GET(native_dump_frame_at_s) > 0 &&
      rex::perf::BenchElapsedMs() >= REXCVAR_GET(native_dump_frame_at_s) * 1000.0 &&
      (dump_all_active_ || pass == 4)) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!frame_open_) return;
    dump_all_active_ = true;
    command_list_->Close();
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), next_fence_value_++);
    DumpFrameResources(fmt::format("p{:02}_", pass), true);
    command_list_->Reset(allocators_[frame_index_].Get(), nullptr);
    frame_state_bound_ = false;
    bound_rt_count_ = 0;
    bound_ds_ = nullptr;
    std::memset(bound_rts_, 0, sizeof(bound_rts_));
    return;
  }
  if (all) return;
  if (frame_dump_done_ || pass != REXCVAR_GET(native_dump_after_pass) ||
      REXCVAR_GET(native_dump_frame_at_s) <= 0 ||
      rex::perf::BenchElapsedMs() < REXCVAR_GET(native_dump_frame_at_s) * 1000.0) {
    return;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!frame_open_) return;
  frame_dump_done_ = true;
  // Submit what the frame recorded so far, dump, then continue recording.
  command_list_->Close();
  ID3D12CommandList* lists[] = {command_list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  queue_->Signal(fence_.Get(), next_fence_value_++);
  DumpFrameResources();
  command_list_->Reset(allocators_[frame_index_].Get(), nullptr);
  frame_state_bound_ = false;
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
  REXLOG_INFO("native: dumped after pass {}", pass);
}

void Renderer::DumpFrameResources(const std::string& prefix, bool surfaces_only,
                                  bool output_only) {
  std::string dir = REXCVAR_GET(native_dump_dir);
  if (dir.empty()) return;
  uint64_t last = next_fence_value_ - 1;
  if (fence_->GetCompletedValue() < last) {
    fence_->SetEventOnCompletion(last, fence_event_);
    WaitForSingleObject(fence_event_, 5000);
  }
  struct Item {
    std::string name;
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES* state;
  };
  std::vector<Item> items;
  for (auto& [key, surf] : surfaces_) {
    if (output_only) break;
    if (surf.samples > 1) continue;  // multisampled: not copyable to a buffer
    char n[96];
    std::snprintf(n, sizeof(n), "surf_%08X_%ux%u_%s", uint32_t(key >> 32), surf.width,
                  surf.height, surf.depth ? "depth" : "color");
    items.push_back({n, surf.resource.Get(), &surf.state});
  }
  if (!surfaces_only && output_rts_[frame_index_]) {
    items.push_back({"output_1280x720", output_rts_[frame_index_].Get(),
                     &output_rt_states_[frame_index_]});
  }
  if (!surfaces_only && !output_only && ssao_ao_) {
    items.push_back({"ssao_ao", ssao_ao_.Get(), &ssao_ao_state_});
  }
  if (REXCVAR_GET(native_debug_texture_format) >= 0 && !surfaces_only) {
    for (auto& [key, te] : textures_) {
      if (!te.resource || te.guest_format != uint32_t(REXCVAR_GET(native_debug_texture_format))) continue;
      D3D12_RESOURCE_DESC rd = te.resource->GetDesc();
      if (rd.Width > 512 || rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) continue;
      char n[96];
      std::snprintf(n, sizeof(n), "tex_%08X_%llux%u", te.guest_base, (unsigned long long)rd.Width,
                    rd.Height);
      te.debug_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      items.push_back({n, te.resource.Get(), &te.debug_state});
    }
  }
  for (auto& [key, rt] : resolve_textures_) {
    if (surfaces_only || output_only) break;
    char n[96];
    std::snprintf(n, sizeof(n), "resolve_%08X_f%u_%ux%u", uint32_t(key >> 32), uint32_t(key) & 0xFF,
                  rt.width, rt.height);
    items.push_back({n, rt.resource.Get(), &rt.state});
  }
  for (Item& item : items) {
    D3D12_RESOURCE_DESC desc = item.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size, total;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &row_size, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = total;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&readback)))) {
      continue;
    }
    allocators_[frame_index_]->Reset();
    command_list_->Reset(allocators_[frame_index_].Get(), nullptr);
    D3D12_RESOURCE_STATES old_state = *item.state;
    Transition(item.resource, *item.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = item.resource;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    command_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(item.resource, *item.state, old_state);
    command_list_->Close();
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    queue_->Signal(fence_.Get(), next_fence_value_);
    fence_->SetEventOnCompletion(next_fence_value_, fence_event_);
    ++next_fence_value_;
    WaitForSingleObject(fence_event_, 5000);
    void* data = nullptr;
    D3D12_RANGE range{0, size_t(total)};
    if (SUCCEEDED(readback->Map(0, &range, &data))) {
      if (std::FILE* f = std::fopen((dir + "/" + prefix + item.name + ".raw").c_str(), "wb")) {
        uint32_t header[4] = {uint32_t(desc.Width), desc.Height, uint32_t(desc.Format),
                              fp.Footprint.RowPitch};
        std::fwrite(header, 4, 4, f);
        std::fwrite(static_cast<uint8_t*>(data) + fp.Offset, 1,
                    size_t(fp.Footprint.RowPitch) * rows, f);
        std::fclose(f);
      }
      readback->Unmap(0, nullptr);
    }
  }
  REXLOG_INFO("native: dumped {} frame resources to {}", items.size(), dir);
}

// ---------------------------------------------------------------------------
// Front end (guest threads): capture + queue. See native_worker.
// ---------------------------------------------------------------------------

void Renderer::BeginCmd(Op op) {
  if (!worker_mode_checked_) {
    worker_mode_checked_ = true;
    worker_mode_ = REXCVAR_GET(native_worker) && REXCVAR_GET(native_pm4_mirror);
    if (worker_mode_) {
      std::thread(&Renderer::WorkerMain, this).detach();
      REXLOG_INFO("native: recording worker enabled");
    }
  }
  if (!batch_) batch_ = std::make_unique<WorkBatch>();
  cur_ = WorkCmd{};
  cur_.op = op;
  cur_.pass = g_guest_pass;
  cur_.range_first = uint32_t(batch_->ranges.size());
  cur_.stream_first = uint32_t(batch_->streams.size());
}

void Renderer::EndCmd(uint8_t* base) {
  cur_.range_count = uint32_t(batch_->ranges.size()) - cur_.range_first;
  cur_.stream_count = uint32_t(batch_->streams.size()) - cur_.stream_first;
  batch_->cmds.push_back(cur_);
  if (!worker_mode_) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Execute(base, *batch_, batch_->cmds.back());
    batch_->Clear();
    return;
  }
  if (batch_->cmds.size() >= 32 || batch_->bytes.size() >= (4u << 20)) {
    FlushBatch();
  }
}

void Renderer::CaptureBytes(uint8_t* base, uint32_t address, uint32_t length) {
  if (!length) return;
  size_t offset = batch_->bytes.size();
  batch_->bytes.resize(offset + length);
  std::memcpy(batch_->bytes.data() + offset, base + address, length);
  batch_->ranges.push_back({address, length, uint32_t(offset)});
}

// Device state the worker reads for a draw/resolve (Load32 through captures):
// device pointer, vertex fetch shadow, register shadow, vertex declaration
// pointer, surface/shader/stream/index pointers and viewport, and the objects
// they point to.
void Renderer::CaptureDevice(uint8_t* base, uint32_t dev) {
  CaptureBytes(base, kDevicePtrAddr, 4);
  CaptureBytes(base, dev, 8);
  CaptureBytes(base, dev + 0x700, 0x80);
  CaptureBytes(base, dev + 0x2880, 0x1D0);
  CaptureBytes(base, dev + 0x2E20, 8);
  CaptureBytes(base, dev + 0x3080, 0x120);
  for (uint32_t i = 0; i < 4; ++i) {
    if (uint32_t rt = Load32(base, dev + kDevRenderTargets + 4 * i)) CaptureBytes(base, rt, 0x28);
  }
  if (uint32_t ds = Load32(base, dev + kDevDepthStencil)) CaptureBytes(base, ds, 0x28);
  if (uint32_t decl = Load32(base, dev + kDevVertexDecl)) {
    uint32_t n = std::min(Load32(base, decl + 0x18), 64u);
    CaptureBytes(base, decl, 0x34 + 12 * n);
  }
}

// PM4 written since the last capture, copied with the command (parsed by
// Execute; direct mode executes the same captured commands synchronously).
void Renderer::CaptureRing(uint8_t* base, uint32_t dev) {
  if (!dev) return;
  // dev+48: last dword written into the current XDK command segment.
  uint32_t current = Load32(base, dev + 48) + 4;
  if (ring_last_ && current >= ring_last_ && current - ring_last_ <= (1u << 20)) {
    uint32_t n = current - ring_last_;
    if (!cur_.ring_bytes) cur_.ring_offset = uint32_t(batch_->bytes.size());
    if (cur_.ring_offset + cur_.ring_bytes == batch_->bytes.size()) {
      batch_->bytes.resize(batch_->bytes.size() + n);
      std::memcpy(batch_->bytes.data() + cur_.ring_offset + cur_.ring_bytes, base + ring_last_, n);
      cur_.ring_bytes += n;
    }
  } else if (ring_last_) {
    ++ring_resyncs_;
  }
  ring_last_ = current;
}

void Renderer::FlushBatch() {
  if (!batch_ || batch_->cmds.empty()) return;
  std::unique_ptr<WorkBatch> next;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    work_queue_.push_back(std::move(batch_));
    ++batches_submitted_;
    if (!free_batches_.empty()) {
      next = std::move(free_batches_.back());
      free_batches_.pop_back();
    }
  }
  queue_cv_.notify_one();
  batch_ = next ? std::move(next) : std::make_unique<WorkBatch>();
}

// Waits until the worker executed the first `batches` batches.
void Renderer::WaitWorkerIdle(uint64_t batches) {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  done_cv_.wait(lock, [this, batches] { return batches_done_ >= batches; });
}

void Renderer::WorkerMain() {
  rex::perf::RegisterSampledThread(4, "native_worker");
  for (;;) {
    std::unique_ptr<WorkBatch> batch;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !work_queue_.empty(); });
      batch = std::move(work_queue_.front());
      work_queue_.pop_front();
    }
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      for (const WorkCmd& cmd : batch->cmds) Execute(guest_base_, *batch, cmd);
    }
    batch->Clear();
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      free_batches_.push_back(std::move(batch));
      ++batches_done_;
    }
    done_cv_.notify_all();
  }
}

void Renderer::Execute(uint8_t* base, const WorkBatch& batch, const WorkCmd& cmd) {
  t_capture_ranges = batch.ranges.data() + cmd.range_first;
  t_capture_count = cmd.range_count;
  t_capture_bytes = batch.bytes.data();
  g_current_pass = cmd.pass;
  if (frame_open_ && REXCVAR_GET(native_gpu_pass_timing)) PassTimestamp(cmd.pass);
  if (cmd.pass != last_exec_pass_) {
    if (fxaa_ && cmd.pass == kPassRenderHud && upscale_rt_ && frame_open_) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      ApplyFxaa(*upscale_rt_);
    }
    if (cmd.pass == kPassRenderHud) upscale_rt_ = nullptr;
    if (ssao_ && !ssao_done_ && cmd.pass >= kPassEndTiling && last_exec_pass_ >= kPassRenderOpaque &&
        last_exec_pass_ < kPassEndTiling && scene_rt_ && scene_ds_ && camera_valid_ && frame_open_) {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      ApplySsao();
      ssao_done_ = true;
    }
    last_exec_pass_ = cmd.pass;
  }
  exec_copies_before_ = mirror_.copy_draws;
  if (cmd.ring_bytes) {
    mirror_.ScanCopy(base, batch.bytes.data() + cmd.ring_offset, cmd.ring_bytes);
  }
  switch (cmd.op) {
    case Op::kDraw:
      ExecDrawVertices(base, batch, cmd);
      break;
    case Op::kDrawIndexed:
      ExecDrawIndexedVertices(base, batch, cmd);
      break;
    case Op::kDrawInline:
      ExecDrawInlineVertices(base, cmd.u[0], cmd.u[1], cmd.u[2], cmd.u[3]);
      break;
    case Op::kResolve:
      ExecResolve(base, cmd.u[0], cmd.u[1], cmd.u[2], cmd.u[3], cmd.u[4], cmd.f, cmd.u[5]);
      break;
    case Op::kBeginTiling:
      ExecBeginTiling(base, cmd.u[0], cmd.u[1], cmd.u[2], cmd.f, cmd.u[3]);
      break;
    case Op::kEndTiling:
      ExecEndTiling();
      break;
    case Op::kSwap:
      ExecOnSwap(base, cmd.u[0], cmd.u64);
      break;
    case Op::kPassEnd:
      ExecOnPassEnd(int(cmd.u[0]));
      break;
    case Op::kClear:
      ExecClear(base, cmd.u[0], cmd.u[1], cmd.u[2], cmd.u[3], cmd.f, cmd.u[4]);
      break;
    case Op::kRing:
      break;
  }
  t_capture_count = 0;
  t_capture_ranges = nullptr;
}

void Renderer::DrawVertices(uint8_t* base, uint32_t prim, uint32_t start_vertex,
                            uint32_t vertex_count) {
  if (!REXCVAR_GET(native_draws)) return;
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  if (prim == uint32_t(xenos::PrimitiveType::kRectangleList)) {
    // Rect lists need CPU expansion: read stream 0 through the guest's
    // physical view (0xA0000000 maps physical memory 1:1) and draw inline.
    uint32_t fetch0 = Load32(base, dev + 0x778);
    uint32_t stride = Load8(base, dev + 0x30E8) * 4;
    uint32_t phys = fetch0 & ~3u;
    if (!phys || !stride) return;
    uint32_t data = 0xA0000000u + phys + start_vertex * stride;
    BeginCmd(Op::kDrawInline);
    CaptureRing(base, dev);
    CaptureDevice(base, dev);
    CaptureBytes(base, data, vertex_count * stride);
    cur_.u[0] = prim;
    cur_.u[1] = data;
    cur_.u[2] = vertex_count;
    cur_.u[3] = stride;
    EndCmd(base);
    return;
  }
  BeginCmd(Op::kDraw);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  cur_.u[0] = prim;
  cur_.u[1] = start_vertex;
  cur_.u[2] = vertex_count;
  VertexRange draw_range;
  draw_range.first = start_vertex;
  draw_range.end = start_vertex + vertex_count;
  cur_.streams_ok = PlanStreams(base, dev, Load32(base, dev + kDevVertexDecl), &draw_range);
  EndCmd(base);
}

void Renderer::DrawIndexedVertices(uint8_t* base, uint32_t prim, int32_t base_vertex,
                                   uint32_t start_index, uint32_t index_count) {
  if (int32_t spin = REXCVAR_GET(native_debug_spin_us)) {
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(spin);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  if (!REXCVAR_GET(native_draws)) return;
  rex::perf::RegisterSampledThread(3, "native_draw_thread");
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  BeginCmd(Op::kDrawIndexed);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  cur_.u[0] = prim;
  cur_.u[1] = uint32_t(base_vertex);
  cur_.u[2] = start_index;
  cur_.u[3] = index_count;
  // Vertex range referenced by the indices, scanned only if a dynamic vertex
  // buffer of this draw is dirty.
  VertexRange draw_range;
  if (uint32_t ib_object = Load32(base, dev + 0x308C)) {
    bool index32 = (Load32(base, ib_object) & 0x80000000u) != 0;
    // +0x18 is a guest *virtual* address: convert like the XDK draw prologue
    // (82580FF8): physical = low 29 bits, +4 KB for the 0xE0000000 view.
    uint32_t v = Load32(base, ib_object + 0x18);
    uint32_t address = (v & 0x1FFFFFFFu) + (v >= 0xE0000000u ? 0x1000u : 0u);
    uint32_t size = Load32(base, ib_object + 0x1C) & 0x00FFFFFFu;
    draw_range.resolved = false;
    draw_range.ib_phys = address;
    draw_range.ib_size = size;
    draw_range.start_index = start_index;
    draw_range.index_count = index_count;
    draw_range.base_vertex = base_vertex;
    draw_range.index32 = index32;
    uint32_t isize = index32 ? 4 : 2;
    bool ok = false;
    cur_.index = PlanBuffer(base, address, size, 0, 0, index32 ? 2 : 1, 0, start_index * isize,
                            (start_index + index_count) * isize, ok);
    cur_.has_index = ok;
    cur_.index32 = index32;
    cur_.index_size = size;
  }
  cur_.streams_ok = PlanStreams(base, dev, Load32(base, dev + kDevVertexDecl), &draw_range);
  EndCmd(base);
}

void Renderer::DrawInlineVertices(uint8_t* base, uint32_t prim, uint32_t data,
                                  uint32_t vertex_count, uint32_t stride) {
  if (!REXCVAR_GET(native_draws)) return;
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  BeginCmd(Op::kDrawInline);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  if (data && stride) CaptureBytes(base, data, vertex_count * stride);
  cur_.u[0] = prim;
  cur_.u[1] = data;
  cur_.u[2] = vertex_count;
  cur_.u[3] = stride;
  EndCmd(base);
}

void Renderer::Resolve(uint8_t* base, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                       uint32_t dest_point, uint32_t clear_color, float clear_z,
                       uint32_t clear_stencil) {
  if (!REXCVAR_GET(native_resolves)) return;
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  BeginCmd(Op::kResolve);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  if (src_rect) CaptureBytes(base, src_rect, 16);
  if (dest_point) CaptureBytes(base, dest_point, 8);
  if (dest_texture) CaptureBytes(base, dest_texture, 0x40);
  if (clear_color) CaptureBytes(base, clear_color, 16);
  cur_.u[0] = flags;
  cur_.u[1] = src_rect;
  cur_.u[2] = dest_texture;
  cur_.u[3] = dest_point;
  cur_.u[4] = clear_color;
  cur_.u[5] = clear_stencil;
  cur_.f = clear_z;
  EndCmd(base);
}

void Renderer::BeginTiling(uint8_t* base, uint32_t count, uint32_t rects, uint32_t clear_color,
                           float clear_z, uint32_t clear_stencil) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  BeginCmd(Op::kBeginTiling);
  if (dev) {
    CaptureRing(base, dev);
    CaptureDevice(base, dev);
  }
  if (rects && count) CaptureBytes(base, rects, 16 * std::min(count, 64u));
  if (clear_color) CaptureBytes(base, clear_color, 16);
  cur_.u[0] = count;
  cur_.u[1] = rects;
  cur_.u[2] = clear_color;
  cur_.u[3] = clear_stencil;
  cur_.f = clear_z;
  EndCmd(base);
}

void Renderer::Clear(uint8_t* base, uint32_t count, uint32_t rects, uint32_t flags,
                     uint32_t color, float z, uint32_t stencil) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  BeginCmd(Op::kClear);
  CaptureRing(base, dev);
  CaptureDevice(base, dev);
  if (rects && count) CaptureBytes(base, rects, 16 * std::min(count, 64u));
  cur_.u[0] = rects ? std::min(count, 64u) : 0;
  cur_.u[1] = rects;
  cur_.u[2] = flags;
  cur_.u[3] = color;
  cur_.u[4] = stencil;
  cur_.f = z;
  EndCmd(base);
}

// XDK clears draw internally (sub_822F9DB8): clear the bound render target 0
// (and its same-EDRAM-base aliases) and the depth-stencil surface directly.
void Renderer::ExecClear(uint8_t* base, uint32_t count, uint32_t rects, uint32_t flags,
                         uint32_t color, float z, uint32_t stencil) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!BeginFrame()) return;
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) return;
  {
    static int logged = 0;
    if (logged < 16) {
      ++logged;
      REXLOG_INFO("native: D3DDevice_Clear flags {:X} color {:08X} z {} stencil {} rects {} pass {}",
                  flags, color, z, stencil, count, g_current_pass);
    }
  }
  std::vector<D3D12_RECT> rs;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t a = rects + 16 * i;
    rs.push_back({LONG(int32_t(Load32(base, a))), LONG(int32_t(Load32(base, a + 4))),
                  LONG(int32_t(Load32(base, a + 8))), LONG(int32_t(Load32(base, a + 12)))});
  }
  UINT nrs = UINT(rs.size());
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(flags & (1u << i))) continue;
    uint32_t object = Load32(base, dev + kDevRenderTargets + 4 * i);
    if (HostSurface* rt = object ? GetSurface(base, object, false) : nullptr) {
      float c[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                    float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f};
      for (auto& [key, other] : surfaces_) {
        if (other.depth || other.edram_base != rt->edram_base || other.width != rt->width ||
            other.height != rt->height || other.scale != rt->scale) {
          continue;
        }
        Transition(other.resource.Get(), other.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        std::vector<D3D12_RECT> scaled = ScaleRects(rs, other.scale);
        command_list_->ClearRenderTargetView(other.view, c, nrs, nrs ? scaled.data() : nullptr);
        MarkWritten(&other);
      }
    }
  }
  D3D12_CLEAR_FLAGS ds_flags = D3D12_CLEAR_FLAGS(0);
  if (flags & kClearZBuffer) ds_flags |= D3D12_CLEAR_FLAG_DEPTH;
  if (flags & kClearStencil) ds_flags |= D3D12_CLEAR_FLAG_STENCIL;
  if (ds_flags) {
    HostSurface* rt0 = GetSurface(base, Load32(base, dev + kDevRenderTargets), false);
    if (HostSurface* ds = GetSurface(base, Load32(base, dev + kDevDepthStencil), true,
                                     rt0 ? rt0->width : 0, rt0 ? rt0->height : 0)) {
      Transition(ds->resource.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      std::vector<D3D12_RECT> scaled = ScaleRects(rs, ds->scale);
      command_list_->ClearDepthStencilView(ds->view, ds_flags, std::clamp(z, 0.0f, 1.0f),
                                           uint8_t(stencil), nrs, nrs ? scaled.data() : nullptr);
      MarkWritten(ds);
    }
  }
  bound_rt_count_ = 0;
  bound_ds_ = nullptr;
  std::memset(bound_rts_, 0, sizeof(bound_rts_));
}

void Renderer::EndTiling() {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (!guest_base_) return;
  BeginCmd(Op::kEndTiling);
  EndCmd(guest_base_);
}

void Renderer::OnPassEnd(int pass) {
  if (REXCVAR_GET(native_dump_after_pass) < 0) return;  // debug dumps only
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (!guest_base_) return;
  BeginCmd(Op::kPassEnd);
  cur_.u[0] = uint32_t(pass);
  EndCmd(guest_base_);
}

void Renderer::SyncRing(uint8_t* base, uint32_t dev) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  BeginCmd(Op::kRing);
  CaptureRing(base, dev);
  EndCmd(base);
}

void Renderer::ResyncRing(uint8_t* base, uint32_t dev) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  if (dev) ring_last_ = Load32(base, dev + 48) + 4;
}

void Renderer::OnSwap(uint8_t* base, uint32_t front_buffer_texture, uint64_t swap_number) {
  std::lock_guard<std::mutex> lock(front_mutex_);
  guest_base_ = base;
  BeginCmd(Op::kSwap);
  CaptureRing(base, Load32(base, kDevicePtrAddr));
  if (front_buffer_texture) CaptureBytes(base, front_buffer_texture, 0x40);
  cur_.u[0] = front_buffer_texture;
  cur_.u64 = swap_number;
  EndCmd(base);
  if (worker_mode_) {
    FlushBatch();
    // native_worker_lag 0: the frame is recorded before the guest continues.
    // 1: the worker may still record this frame while the guest builds the
    // next one (everything a command reads from guest memory is captured,
    // except texture data - watched - and shader literal tables).
    uint64_t submitted;
    {
      std::lock_guard<std::mutex> qlock(queue_mutex_);
      submitted = batches_submitted_;
    }
    WaitWorkerIdle(REXCVAR_GET(native_worker_lag) ? prev_swap_batches_ : submitted);
    prev_swap_batches_ = submitted;
  }
}

void Renderer::ExecOnSwap(uint8_t* base, uint32_t front_buffer_texture, uint64_t swap_number) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  swap_number_ = swap_number;
  // Per-frame SSAO / soft particle state.
  scene_rt_ = scene_ds_ = nullptr;
  scene_depth_key_ = 0;
  camera_valid_ = ssao_done_ = false;
  if (REXCVAR_GET(native_dump_swap) > 0 && swap_number == uint64_t(REXCVAR_GET(native_dump_swap))) {
    mirror_snapshot_.assign(mirror_.regs(), mirror_.regs() + Pm4Mirror::kRegisterCount);
  }
  if (!BeginFrame()) {
    return;
  }
  uint32_t front_buffer_address = 0;
  if (front_buffer_texture) {
    front_buffer_address = GuestPhysical(Load32(base, front_buffer_texture + 0x1C + 4) & 0xFFFFF000u);
  }
  EndFrameAndPresent(front_buffer_address);
}


}  // namespace conan::native
