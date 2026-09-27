#include "shader_registry.h"

#include <mutex>
#include <unordered_map>

#include <rex/hash.h>
#include <rex/ppc.h>

namespace conan::native {

namespace {
std::mutex g_mutex;
std::unordered_map<uint32_t, GuestShaderInfo> g_shaders;

inline uint32_t Load32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + addr));
}

// Container layout (XenosRecomp ShaderContainer): +0 flags (0x102A1100 PS /
// 0x102A1101 VS), +4 virtualSize, +8 physicalSize; the microcode (physical
// part) directly follows the virtual part, so the whole container is
// contiguous: hash [c, c + virtual + physical) exactly like the corpus.
GuestShaderInfo HashContainer(uint8_t* base, uint32_t container) {
  GuestShaderInfo info;
  uint32_t flags = Load32(base, container);
  uint32_t virtual_size = Load32(base, container + 4);
  uint32_t physical_size = Load32(base, container + 8);
  info.is_vertex = (flags & 1) != 0;
  info.container_hash = XXH3_64bits(base + container, size_t(virtual_size) + physical_size);
  return info;
}
}  // namespace

const GuestShaderInfo* LookupGuestShader(uint32_t guest_object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_shaders.find(guest_object);
  return it == g_shaders.end() ? nullptr : &it->second;
}

}  // namespace conan::native

// XDK shader creators (called by the D3DX effect loader 825B2E68): r3 =
// shader container, returns the new D3D shader object in r3.
#define CONAN_SHADER_CREATE_HOOK(addr)                                          \
  REX_EXTERN(__imp__sub_##addr);                                                \
  extern "C" REX_FUNC(sub_##addr) {                                             \
    conan::native::GuestShaderInfo info =                                       \
        conan::native::HashContainer(base, ctx.r3.u32);                         \
    __imp__sub_##addr(ctx, base);                                               \
    if (ctx.r3.u32) {                                                           \
      std::lock_guard<std::mutex> lock(conan::native::g_mutex);                 \
      conan::native::g_shaders[ctx.r3.u32] = info;                              \
    }                                                                           \
  }

namespace conan::native {
// Expose internals to the hook macro.
}
CONAN_SHADER_CREATE_HOOK(822E84C0)
CONAN_SHADER_CREATE_HOOK(822E85D0)
