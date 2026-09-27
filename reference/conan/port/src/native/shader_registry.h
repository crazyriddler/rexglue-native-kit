// Guest shader object -> shader corpus identity (container_hash, the key used
// by artifacts/shaders/catalog.json and XenosRecomp).
#pragma once

#include <cstdint>

namespace conan::native {

struct GuestShaderInfo {
  uint64_t container_hash = 0;
  bool is_vertex = false;
};

// Returns nullptr for objects not created through the hooked XDK creators.
const GuestShaderInfo* LookupGuestShader(uint32_t guest_object);

}  // namespace conan::native
