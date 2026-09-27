// Guest D3D workload capture (milestone M3).
//
// Pass-through hooks on the game's statically linked XDK Direct3D entry points
// and on each render-pass function of the game's pass table (0x82A25A70, see
// docs/RENDERER_ANALYSIS.md). Every hook calls the original (`__imp__`) so the
// legacy Xenos path renders exactly as before; the hooks only observe the
// guest D3DDevice shadow state at draw time and aggregate it:
//
//   * per-pass draw/resolve counts (averaged per frame),
//   * deduplicated "draw signatures" (pass, primitive, VS/PS object, vertex
//     declaration, render-target/depth surface formats, render-state register
//     shadow hash, bound texture count) - the set of pipelines a native
//     renderer has to cover,
//   * app0 = guest draw calls per frame, app1 = guest resolves per frame in the
//     perf CSV, so the counts can be checked against the Xenos backend's own
//     draw counter (docs/OPEN_QUESTIONS.md Q-R1).
//
// Enabled with --d3d_capture_out=<path.json>; costs one branch per hook when
// off. The JSON is written by the bench exit callback.

#include <rex/cvar.h>
#include <rex/perf/counter.h>
#include <rex/ppc.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <set>
#include <tuple>

#include "native_renderer.h"
#include "shader_registry.h"
#include "texture_decode.h"

REXCVAR_DEFINE_STRING(d3d_capture_out, "", "Conan",
                      "Write a JSON catalog of guest D3D draws/passes to this path at bench exit");
REXCVAR_DEFINE_STRING(native_dump_textures_dir, "", "Conan",
                      "Decode (CPU, native path) every distinct texture bound during capture and "
                      "write it as DDS into this directory (texture decoder validation)");

namespace {

constexpr uint32_t kDevicePtrAddr = 0x82C81A64;

// D3DDevice offsets (docs/RENDERER_ANALYSIS.md section 4).
constexpr uint32_t kDevRenderTargets = 0x3090;
constexpr uint32_t kDevDepthStencil = 0x30A0;
constexpr uint32_t kDevVertexDecl = 0x2E24;
constexpr uint32_t kDevTextures = 0x30F8;
constexpr uint32_t kDevVertexShader = 0x318C;
constexpr uint32_t kDevPixelShader = 0x3190;
constexpr uint32_t kDevRegShadowBegin = 0x2900;
constexpr uint32_t kDevRegShadowEnd = 0x2A00;

const char* const kPassNames[] = {
    "none",           "Gather Batches",   "Begin Game Render", "Render Level Heightmap",
    "Render Shadow Maps", "Calc Light Parameters", "Render Reflections", "Begin Tiling",
    "Render Depth Only", "Render Decal",   "Render Character ID", "Deferred Shade",
    "Render Opaque",  "Render Opaque Character", "Render Skybox", "Render Black River Serpent",
    "End Tiling",     "Render Ocean",     "Copy Back Buffer",  "Render Painters Edge",
    "Resolve HDR Texture", "Render Sorted", "End Game Render", "Render Post Process",
    "Render Debug",   "Upscale",          "Render Movie",      "Render HUD",
    "Present",
};
constexpr int kNumPasses = int(sizeof(kPassNames) / sizeof(kPassNames[0]));

bool g_enabled = false;
bool g_checked = false;
int g_pass = 0;
bool g_in_draw_up = false;  // DrawVerticesUP calls BeginVertices internally

struct PassStats {
  uint64_t draws = 0;
  uint64_t draws_indexed = 0;
  uint64_t draws_up = 0;
  uint64_t draws_begin = 0;
  uint64_t resolves = 0;
  uint64_t primitives = 0;
};

struct Signature {
  int pass;
  uint32_t prim;
  uint32_t vs, ps, decl;
  uint64_t vs_hash, ps_hash;
  uint32_t rt_info[4];
  uint32_t ds_info;
  uint64_t state_hash;
  uint32_t texture_count;
  bool operator<(const Signature& o) const {
    return std::memcmp(this, &o, sizeof(*this)) < 0;
  }
};

std::mutex g_mutex;
PassStats g_pass_stats[kNumPasses];
std::map<Signature, uint64_t> g_signatures;
// Distinct render-target/depth surface objects (12 dwords) and resolve call shapes.
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint64_t> g_surfaces;
struct ResolveKey {
  int pass;
  uint32_t flags, src_rect[4], dest_fetch[6], rt_info, level, slice;
  bool operator<(const ResolveKey& o) const { return std::memcmp(this, &o, sizeof(*this)) < 0; }
};
std::map<ResolveKey, uint64_t> g_resolves;
uint64_t g_frames = 0;
uint64_t g_frames_at_capture_start = 0;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + addr));
}

void DumpCapture();

bool Enabled() {
  if (!g_checked) {
    g_checked = true;
    g_enabled = !REXCVAR_GET(d3d_capture_out).empty();
    if (g_enabled) {
      rex::perf::RegisterBenchExitCallback(DumpCapture);
    }
  }
  return g_enabled;
}

// Only aggregate once the benchmark's measured window has started, so the
// catalog reflects gameplay rather than menus/loading.
bool Capturing() {
  return rex::perf::BenchElapsedMs() >= 50000.0;
}

uint64_t HashRange(uint8_t* base, uint32_t begin, uint32_t end) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t a = begin; a < end; a += 4) {
    h ^= *reinterpret_cast<uint32_t*>(base + a);
    h *= 1099511628211ull;
  }
  return h;
}

uint32_t SurfaceInfo(uint8_t* base, uint32_t surface) {
  return surface ? Load32(base, surface + 0x1C) : 0;
}

void DumpBoundTextures(uint8_t* base, uint32_t dev) {
  static const std::string dir = REXCVAR_GET(native_dump_textures_dir);
  if (dir.empty()) {
    return;
  }
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
  static int failures_logged = 0;
  for (int i = 0; i < 26; ++i) {
    uint32_t tex = Load32(base, dev + kDevTextures + 4 * i);
    if (!tex) continue;
    uint32_t fetch[6];
    for (int d = 0; d < 6; ++d) fetch[d] = Load32(base, tex + 0x1C + 4 * d);
    auto key = std::make_tuple(fetch[1], fetch[2], fetch[3]);
    if (!seen.insert(key).second) continue;
    conan::native::DecodedTexture decoded;
    const char* reason = nullptr;
    if (!conan::native::DecodeTexture(fetch, decoded, &reason)) {
      if (failures_logged++ < 64) {
        std::FILE* f = std::fopen((dir + "/failures.txt").c_str(), "a");
        if (f) {
          std::fprintf(f, "pass=%s tex=%08X fetch=%08X %08X %08X %08X %08X %08X: %s\n",
                       kPassNames[g_pass], tex, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4],
                       fetch[5], reason ? reason : "?");
          std::fclose(f);
        }
      }
      continue;
    }
    char name[512];
    std::snprintf(name, sizeof(name), "%s/%s_%08X_%ux%u_f%u.dds", dir.c_str(),
                  kPassNames[g_pass], decoded.base_address, decoded.width, decoded.height,
                  decoded.guest_format);
    for (char* c = name + dir.size() + 1; *c; ++c) {
      if (*c == ' ') *c = '_';
    }
    conan::native::WriteDds(decoded, name);
    if (std::FILE* f = std::fopen((dir + "/fetch.txt").c_str(), "a")) {
      std::fprintf(f, "%s %08X %08X %08X %08X %08X %08X\n", name + dir.size() + 1, fetch[0],
                   fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
      std::fclose(f);
    }
  }
}

void RecordDraw(uint8_t* base, uint32_t prim, uint32_t prim_count, int kind) {
  if (!Capturing()) {
    return;
  }
  uint32_t dev = Load32(base, kDevicePtrAddr);
  if (!dev) {
    return;
  }
  Signature sig{};
  sig.pass = g_pass;
  sig.prim = prim;
  sig.vs = Load32(base, dev + kDevVertexShader);
  sig.ps = Load32(base, dev + kDevPixelShader);
  sig.decl = Load32(base, dev + kDevVertexDecl);
  if (auto* info = conan::native::LookupGuestShader(sig.vs)) sig.vs_hash = info->container_hash;
  if (auto* info = conan::native::LookupGuestShader(sig.ps)) sig.ps_hash = info->container_hash;
  for (int i = 0; i < 5; ++i) {
    uint32_t surf = Load32(base, dev + (i < 4 ? kDevRenderTargets + 4 * i : kDevDepthStencil));
    if (i < 4) sig.rt_info[i] = SurfaceInfo(base, surf);
    if (surf) {
      std::lock_guard<std::mutex> lock(g_mutex);
      ++g_surfaces[std::make_tuple(uint32_t(i), Load32(base, surf + 0x18), Load32(base, surf + 0x1C),
                                   Load32(base, surf + 0x24), Load32(base, surf + 0x28))];
    }
  }
  sig.ds_info = SurfaceInfo(base, Load32(base, dev + kDevDepthStencil));
  sig.state_hash = HashRange(base, dev + kDevRegShadowBegin, dev + kDevRegShadowEnd);
  for (int i = 0; i < 26; ++i) {
    sig.texture_count += Load32(base, dev + kDevTextures + 4 * i) != 0;
  }
  DumpBoundTextures(base, dev);
  std::lock_guard<std::mutex> lock(g_mutex);
  PassStats& ps = g_pass_stats[g_pass];
  ++ps.draws;
  ps.draws_indexed += kind == 1;
  ps.draws_up += kind == 2;
  ps.draws_begin += kind == 3;
  ps.primitives += prim_count;
  ++g_signatures[sig];
}

const char* PrimName(uint32_t prim) {
  switch (prim) {
    case 1: return "pointlist";
    case 2: return "linelist";
    case 3: return "linestrip";
    case 4: return "trianglelist";
    case 5: return "trianglefan";
    case 6: return "trianglestrip";
    case 8: return "rectlist";
    case 13: return "quadlist";
    default: return "other";
  }
}

void DumpCapture() {
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::string& path = REXCVAR_GET(d3d_capture_out);
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    return;
  }
  uint64_t frames = g_frames - g_frames_at_capture_start;
  if (!frames) frames = 1;
  std::fprintf(f, "{\n  \"frames\": %llu,\n  \"passes\": [\n", (unsigned long long)frames);
  bool first = true;
  for (int i = 0; i < kNumPasses; ++i) {
    const PassStats& p = g_pass_stats[i];
    if (!p.draws && !p.resolves) continue;
    std::fprintf(f,
                 "%s    {\"pass\": \"%s\", \"draws_per_frame\": %.2f, \"indexed\": %.2f, "
                 "\"up\": %.2f, \"begin\": %.2f, \"resolves_per_frame\": %.2f, \"prims_per_frame\": %.1f}",
                 first ? "" : ",\n", kPassNames[i], double(p.draws) / frames,
                 double(p.draws_indexed) / frames, double(p.draws_up) / frames,
                 double(p.draws_begin) / frames,
                 double(p.resolves) / frames, double(p.primitives) / frames);
    first = false;
  }
  std::fprintf(f, "\n  ],\n  \"signatures\": [\n");
  first = true;
  for (auto& [s, count] : g_signatures) {
    std::fprintf(f,
                 "%s    {\"pass\": \"%s\", \"prim\": \"%s\", \"vs\": \"%08X\", \"ps\": \"%08X\", "
                 "\"vs_hash\": \"%016llX\", \"ps_hash\": \"%016llX\", "
                 "\"decl\": \"%08X\", \"rt\": [\"%08X\",\"%08X\",\"%08X\",\"%08X\"], "
                 "\"ds\": \"%08X\", \"state\": \"%016llX\", \"textures\": %u, "
                 "\"draws_per_frame\": %.3f}",
                 first ? "" : ",\n", kPassNames[s.pass], PrimName(s.prim), s.vs, s.ps,
                 (unsigned long long)s.vs_hash, (unsigned long long)s.ps_hash, s.decl,
                 s.rt_info[0], s.rt_info[1], s.rt_info[2], s.rt_info[3], s.ds_info,
                 (unsigned long long)s.state_hash, s.texture_count, double(count) / frames);
    first = false;
  }
  std::fprintf(f, "\n  ],\n  \"surfaces\": [\n");
  first = true;
  for (auto& [k, count] : g_surfaces) {
    std::fprintf(f,
                 "%s    {\"slot\": %u, \"surface_info\": \"%08X\", \"color_depth_info\": \"%08X\", "
                 "\"size_bits\": \"%08X\", \"format\": \"%08X\", \"draws\": %llu}",
                 first ? "" : ",\n", std::get<0>(k), std::get<1>(k), std::get<2>(k),
                 std::get<3>(k), std::get<4>(k), (unsigned long long)count);
    first = false;
  }
  std::fprintf(f, "\n  ],\n  \"resolves\": [\n");
  first = true;
  for (auto& [k, count] : g_resolves) {
    std::fprintf(f,
                 "%s    {\"pass\": \"%s\", \"flags\": \"%08X\", \"src_rect\": [%d,%d,%d,%d], "
                 "\"dest_fetch\": [\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\",\"%08X\"], "
                 "\"rt_info\": \"%08X\", \"level\": %u, \"slice\": %u, \"per_frame\": %.2f}",
                 first ? "" : ",\n", kPassNames[k.pass], k.flags, int(k.src_rect[0]),
                 int(k.src_rect[1]), int(k.src_rect[2]), int(k.src_rect[3]), k.dest_fetch[0],
                 k.dest_fetch[1], k.dest_fetch[2], k.dest_fetch[3], k.dest_fetch[4],
                 k.dest_fetch[5], k.rt_info, k.level, k.slice, double(count) / frames);
    first = false;
  }
  std::fprintf(f, "\n  ]\n}\n");
  std::fclose(f);
}

struct PassScope {
  int saved;
  explicit PassScope(int pass) : saved(g_pass) {
    g_pass = pass;
    conan::native::g_guest_pass = pass;
  }
  ~PassScope() {
    if (conan::native::Enabled()) {
      conan::native::Renderer::Get().OnPassEnd(g_pass);
    }
    g_pass = saved;
    conan::native::g_guest_pass = saved;
  }
};

// Inline-vertex draw opened by BeginVertices, consumed by EndVertices.
struct PendingInlineDraw {
  bool active = false;
  uint32_t prim = 0, count = 0, stride = 0, data = 0;
};
PendingInlineDraw g_pending_inline;

}  // namespace

// ---- Render-pass hooks -----------------------------------------------------

#define CONAN_PASS_HOOK(addr, index)                   \
  REX_EXTERN(__imp__sub_##addr);                       \
  extern "C" REX_FUNC(sub_##addr) {                    \
    PassScope scope(index);     \
    __imp__sub_##addr(ctx, base);                      \
  }

CONAN_PASS_HOOK(824F1848, 1)
CONAN_PASS_HOOK(824F20D8, 2)
CONAN_PASS_HOOK(824F1850, 3)
CONAN_PASS_HOOK(824F1880, 4)
CONAN_PASS_HOOK(824F1A38, 5)
CONAN_PASS_HOOK(824F19E8, 6)
CONAN_PASS_HOOK(824F1A40, 7)
CONAN_PASS_HOOK(824F1AD0, 8)
CONAN_PASS_HOOK(824F1B38, 9)
CONAN_PASS_HOOK(824F1B88, 10)
CONAN_PASS_HOOK(824F21D8, 11)
CONAN_PASS_HOOK(824F1BE8, 12)
CONAN_PASS_HOOK(824F1C38, 13)
CONAN_PASS_HOOK(824F1CA8, 14)
CONAN_PASS_HOOK(824F1CB8, 15)
CONAN_PASS_HOOK(824F1CC0, 16)
CONAN_PASS_HOOK(824F1CB0, 17)
CONAN_PASS_HOOK(824F1D60, 18)
CONAN_PASS_HOOK(824F1D78, 19)
CONAN_PASS_HOOK(824F1D80, 20)
CONAN_PASS_HOOK(824F1DD8, 21)
CONAN_PASS_HOOK(824F2248, 22)
CONAN_PASS_HOOK(824F1DE0, 23)
CONAN_PASS_HOOK(824F2290, 24)
CONAN_PASS_HOOK(824F1E38, 25)
CONAN_PASS_HOOK(824F1E40, 26)
CONAN_PASS_HOOK(824F1E68, 27)

// Renderer::Present (resolve to front buffer + Swap). Also the frame boundary.
REX_EXTERN(__imp__sub_82533178);
extern "C" REX_FUNC(sub_82533178) {
  if (Enabled()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_frames;
    if (!Capturing()) {
      g_frames_at_capture_start = g_frames;
    }
  }
  PassScope scope(28);
  __imp__sub_82533178(ctx, base);
}

// ---- D3D API hooks ---------------------------------------------------------

// D3DDevice_DrawVertices(dev, PrimType, StartVertex, VertexCount)
REX_EXTERN(__imp__sub_82580918);
extern "C" REX_FUNC(sub_82580918) {
  if (Enabled()) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kApp0);
    RecordDraw(base, ctx.r4.u32, ctx.r6.u32, 0);
  }
  // The original runs first: it flushes the draw's dirty state into the
  // command buffer, which the native PM4 mirror parses.
  uint32_t prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_82580918(ctx, base);
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().DrawVertices(base, prim, start, count);
  }
}

// D3DDevice_DrawIndexedVertices(dev, PrimType, BaseVertexIndex, StartIndex, IndexCount)
REX_EXTERN(__imp__sub_82580D00);
extern "C" REX_FUNC(sub_82580D00) {
  if (Enabled()) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kApp0);
    RecordDraw(base, ctx.r4.u32, ctx.r7.u32, 1);
  }
  uint32_t prim = ctx.r4.u32, base_vertex = ctx.r5.u32, start = ctx.r6.u32, count = ctx.r7.u32;
  __imp__sub_82580D00(ctx, base);
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().DrawIndexedVertices(base, prim, int32_t(base_vertex), start,
                                                       count);
  }
}

// D3DDevice_DrawVerticesUP(dev, PrimType, VertexCount, pData, Stride) - Begin/End based.
REX_EXTERN(__imp__sub_825808B8);
extern "C" REX_FUNC(sub_825808B8) {
  if (Enabled()) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kApp0);
    RecordDraw(base, ctx.r4.u32, ctx.r5.u32, 2);
  }
  uint32_t prim = ctx.r4.u32, count = ctx.r5.u32, data = ctx.r6.u32, stride = ctx.r7.u32;
  g_in_draw_up = true;
  __imp__sub_825808B8(ctx, base);
  g_in_draw_up = false;
  if (conan::native::Enabled()) {
    // DrawVerticesUP(dev, PrimType, VertexCount, pVertexData, Stride)
    conan::native::Renderer::Get().DrawInlineVertices(base, prim, data, count, stride);
  }
}

// D3DDevice_BeginVertices(dev, PrimType, VertexCount, Stride) - direct callers
// only (DrawVerticesUP's internal use is attributed above and counted twice in
// app2, which is fine for a sanity check).
REX_EXTERN(__imp__sub_825803F8);
extern "C" REX_FUNC(sub_825803F8) {
  if (Enabled() && !g_in_draw_up) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kApp0);
    rex::perf::IncrementCounter(rex::perf::CounterId::kApp2);
    RecordDraw(base, ctx.r4.u32, ctx.r5.u32, 3);
  }
  uint32_t prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__sub_825803F8(ctx, base);
  if (conan::native::Enabled() && !g_in_draw_up) {
    // r3 = where the game writes the inline vertices (inside the ring).
    g_pending_inline = {true, prim, count, stride, ctx.r3.u32};
  }
}

// D3DDevice_Resolve(dev, Flags, pSrcRect, pDestTexture, ...)
REX_EXTERN(__imp__sub_822F5028);
extern "C" REX_FUNC(sub_822F5028) {
  if (Enabled()) {
    rex::perf::IncrementCounter(rex::perf::CounterId::kApp1);
    if (Capturing()) {
      ResolveKey k{};
      k.pass = g_pass;
      k.flags = ctx.r4.u32;
      if (ctx.r5.u32) for (int i = 0; i < 4; ++i) k.src_rect[i] = Load32(base, ctx.r5.u32 + 4 * i);
      if (ctx.r6.u32) for (int i = 0; i < 6; ++i) k.dest_fetch[i] = Load32(base, ctx.r6.u32 + 0x1C + 4 * i);
      uint32_t dev = Load32(base, kDevicePtrAddr);
      uint32_t idx = ctx.r4.u32 & 7;
      uint32_t surf = dev ? Load32(base, dev + (idx < 4 ? kDevRenderTargets + 4 * idx : kDevDepthStencil)) : 0;
      k.rt_info = SurfaceInfo(base, surf);
      k.level = ctx.r8.u32;
      k.slice = ctx.r9.u32;
      std::lock_guard<std::mutex> lock(g_mutex);
      ++g_pass_stats[g_pass].resolves;
      ++g_resolves[k];
    }
  }
  // Original first: the resolve's RB_COPY_* registers land in the command
  // stream the native PM4 mirror parses.
  uint32_t flags = ctx.r4.u32, rect = ctx.r5.u32, dest = ctx.r6.u32, point = ctx.r7.u32,
           clear_color = ctx.r10.u32;
  float clear_z = float(ctx.f1.f64);
  __imp__sub_822F5028(ctx, base);
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().Resolve(base, flags, rect, dest, point, clear_color, clear_z, 0);
  }
}

// D3DDevice_EndVertices(dev): the inline vertex data is complete here.
REX_EXTERN(__imp__sub_82580898);
extern "C" REX_FUNC(sub_82580898) {
  if (g_pending_inline.active) {
    g_pending_inline.active = false;
    conan::native::Renderer::Get().DrawInlineVertices(base, g_pending_inline.prim,
                                                      g_pending_inline.data,
                                                      g_pending_inline.count,
                                                      g_pending_inline.stride);
  }
  __imp__sub_82580898(ctx, base);
}

// D3DDevice_BeginTiling(dev, Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil)
REX_EXTERN(__imp__sub_822F3EF8);
extern "C" REX_FUNC(sub_822F3EF8) {
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().BeginTiling(base, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                                               float(ctx.f1.f64), ctx.r8.u32);
  }
  __imp__sub_822F3EF8(ctx, base);
}

// D3DDevice_EndTiling: its per-tile resolves go through the Resolve hook.
REX_EXTERN(__imp__sub_822F4480);
extern "C" REX_FUNC(sub_822F4480) {
  __imp__sub_822F4480(ctx, base);
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().EndTiling();
  }
}

// D3DVertexBuffer_Unlock / D3DIndexBuffer_Unlock (r3 = buffer object): the
// guest has rewritten the buffer contents.
static void InvalidateBufferObject(uint8_t* base, uint32_t object) {
  if (!object || !conan::native::Enabled()) return;
  uint32_t address = Load32(base, object + 0x18) & ~3u;
  uint32_t size = Load32(base, object + 0x1C) & 0x00FFFFFFu;
  conan::native::Renderer::Get().InvalidateGuestRange(address, size ? size : 0x10000);
}
REX_EXTERN(__imp__sub_822EA0D8);
extern "C" REX_FUNC(sub_822EA0D8) {
  uint32_t object = ctx.r3.u32;
  __imp__sub_822EA0D8(ctx, base);
  InvalidateBufferObject(base, object);
}
REX_EXTERN(__imp__sub_822EA1E0);
extern "C" REX_FUNC(sub_822EA1E0) {
  uint32_t object = ctx.r3.u32;
  __imp__sub_822EA1E0(ctx, base);
  InvalidateBufferObject(base, object);
}

// D3DDevice_GpuBeginShaderConstantF4(dev, bPixelShader, StartRegister,
//   ppCachedConstants, ppWriteCombinedConstants, Vector4fCount): the game
// writes constants through the returned ring pointer only.
// Shader literal constants: when a shader is bound (sub_822F75F8), the XDK
// emits PM4 LOAD_ALU_CONSTANT packets that load the shader's "def" constants
// straight from the shader object into the GPU constant file, bypassing the
// device's constant shadow. sub_822F7408(dev, table, data_base) walks
// {u16 vec4 register, u16 dword count, u32 offset} entries.
// Inline shader constant upload: sub_82580358(dev, r4, r5, vec4_count) reserves
// a PM4 SET_CONSTANT packet in the ring and returns the data pointer the game
// fills (e.g. the per-object light constants c64+ of the scene shaders). The
// ALU register is ((r5 - (r4 << 8)) * 4) & 0x7FC dwords (VS 0-255, PS 256-511).
// XDK command segment switch (segment full / kickoff): the native PM4 mirror
// parses the tail of the old segment and resynchronizes on the new one.
REX_EXTERN(__imp__sub_822DF848);
extern "C" REX_FUNC(sub_822DF848) {
  uint32_t dev = ctx.r3.u32;
  if (conan::native::Enabled()) conan::native::Renderer::Get().SyncRing(base, dev);
  __imp__sub_822DF848(ctx, base);
  if (conan::native::Enabled()) conan::native::Renderer::Get().ResyncRing(base, dev);
}

// Large command segment allocation (after sub_822DF848 could not satisfy it).
REX_EXTERN(__imp__sub_822DF548);
extern "C" REX_FUNC(sub_822DF548) {
  uint32_t dev = ctx.r3.u32;
  if (conan::native::Enabled()) conan::native::Renderer::Get().SyncRing(base, dev);
  __imp__sub_822DF548(ctx, base);
  if (conan::native::Enabled()) conan::native::Renderer::Get().ResyncRing(base, dev);
}

REX_EXTERN(__imp__sub_82580358);
extern "C" REX_FUNC(sub_82580358) {
  uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_82580358(ctx, base);
  if (conan::native::Enabled() && ctx.r3.u32) {
    uint32_t unified = (r5 - (r4 << 8)) & 0x1FF;
    conan::native::Renderer::Get().NoteRingConstants(unified >= 256, unified & 0xFF, count,
                                                      ctx.r3.u32);
  }
}

REX_EXTERN(__imp__sub_822F7408);
extern "C" REX_FUNC(sub_822F7408) {
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().ApplyLoadAluConstants(base, ctx.r3.u32, ctx.r4.u32,
                                                         ctx.r5.u32);
  }
  __imp__sub_822F7408(ctx, base);
}

REX_EXTERN(__imp__sub_822E7A48);
extern "C" REX_FUNC(sub_822E7A48) {
  bool pixel = ctx.r4.u32 != 0;
  uint32_t start = ctx.r5.u32, ring_out = ctx.r7.u32, count = ctx.r8.u32;
  __imp__sub_822E7A48(ctx, base);
  if (conan::native::Enabled() && ring_out) {
    conan::native::Renderer::Get().NoteRingConstants(pixel, start, count, Load32(base, ring_out));
  }
}

// D3DDevice_Clear(dev, Count, pRects, Flags, Color, Z, Stencil) - XDK clears
// through an internal draw (sub_822F9DB8, shared with BeginTiling's clear),
// never through the hooked draw entry points.
REX_EXTERN(__imp__sub_822F9EE0);
extern "C" REX_FUNC(sub_822F9EE0) {
  uint32_t count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32, color = ctx.r7.u32,
           stencil = ctx.r9.u32;
  float z = float(ctx.f1.f64);
  __imp__sub_822F9EE0(ctx, base);
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().Clear(base, count, rects, flags, color, z, stencil);
  }
}
