// CPU-side decode of guest Xenos textures (fetch constant -> linear host data).
// Uses the SDK's exported Xenos texture layout/untile helpers (the same math
// the legacy Xenos path relies on), so only the host format mapping is new.
#pragma once

#include <cstdint>
#include <vector>

#include <rex/ui/d3d12/d3d12_api.h>

namespace conan::native {

struct DecodedTexture {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;       // array/stack size for 2D arrays, depth for 3D
  uint32_t mip_levels = 1;
  uint32_t dimension = 1;   // xenos::DataDimension: 0=1D 1=2D 2=3D 3=cube
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  // D3D12 Shader4ComponentMapping derived from the fetch swizzle.
  uint32_t component_mapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  uint32_t guest_format = 0;
  uint32_t base_address = 0;
  uint32_t base_size = 0;  // guest bytes of the base level (all slices/faces)
  struct Level {
    uint32_t width, height;
    uint32_t row_pitch;   // bytes per row of blocks (tightly packed)
    uint32_t rows;        // rows of blocks
    size_t offset;        // into data
  };
  std::vector<Level> levels;  // per mip (and per array slice: slice-major)
  std::vector<uint8_t> data;
};

// fetch: the 6 fetch-constant dwords in host byte order.
// Returns false (with a reason) for formats/dimensions not handled yet.
bool DecodeTexture(const uint32_t fetch[6], DecodedTexture& out, const char** reason = nullptr);
// Guest memory range of the base level (what DecodeTexture reports as
// base_address/base_size), without decoding.
bool GetTextureBaseRange(const uint32_t fetch[6], uint32_t& base, uint32_t& size);

// Debug helper: writes a DDS file (DX10 header) of the decoded data.
bool WriteDds(const DecodedTexture& tex, const char* path);

}  // namespace conan::native
