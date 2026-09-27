#include "texture_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace conan::native {

namespace xenos = rex::graphics::xenos;
using rex::graphics::FormatInfo;
using rex::graphics::TextureExtent;
using rex::graphics::TextureInfo;

namespace {

struct HostFormat {
  DXGI_FORMAT format;
  // Guest data needs per-block conversion before upload (unsupported for now).
  bool supported;
};

HostFormat MapFormat(xenos::TextureFormat f) {
  using TF = xenos::TextureFormat;
  switch (f) {
    case TF::k_8:
    case TF::k_8_A:
    case TF::k_8_B:
      return {DXGI_FORMAT_R8_UNORM, true};
    case TF::k_8_8:
      return {DXGI_FORMAT_R8G8_UNORM, true};
    case TF::k_8_8_8_8:
    case TF::k_8_8_8_8_A:
      return {DXGI_FORMAT_R8G8B8A8_UNORM, true};
    case TF::k_2_10_10_10:
      return {DXGI_FORMAT_R10G10B10A2_UNORM, true};
    case TF::k_5_6_5:
      return {DXGI_FORMAT_B5G6R5_UNORM, true};
    case TF::k_1_5_5_5:
      return {DXGI_FORMAT_B5G5R5A1_UNORM, true};
    case TF::k_4_4_4_4:
      return {DXGI_FORMAT_B4G4R4A4_UNORM, true};
    case TF::k_DXT1:
      return {DXGI_FORMAT_BC1_UNORM, true};
    case TF::k_DXT2_3:
      return {DXGI_FORMAT_BC2_UNORM, true};
    case TF::k_DXT4_5:
      return {DXGI_FORMAT_BC3_UNORM, true};
    case TF::k_DXN:
      return {DXGI_FORMAT_BC5_UNORM, true};
    case TF::k_16:
      return {DXGI_FORMAT_R16_UNORM, true};
    case TF::k_16_16:
      return {DXGI_FORMAT_R16G16_UNORM, true};
    case TF::k_16_16_16_16:
      return {DXGI_FORMAT_R16G16B16A16_UNORM, true};
    // _EXPAND formats hold 16-bit floats (Xenia maps them the same way).
    case TF::k_16_EXPAND:
      return {DXGI_FORMAT_R16_FLOAT, true};
    case TF::k_16_16_EXPAND:
      return {DXGI_FORMAT_R16G16_FLOAT, true};
    case TF::k_16_16_16_16_EXPAND:
      return {DXGI_FORMAT_R16G16B16A16_FLOAT, true};
    case TF::k_16_FLOAT:
      return {DXGI_FORMAT_R16_FLOAT, true};
    case TF::k_16_16_FLOAT:
      return {DXGI_FORMAT_R16G16_FLOAT, true};
    case TF::k_16_16_16_16_FLOAT:
      return {DXGI_FORMAT_R16G16B16A16_FLOAT, true};
    case TF::k_32_FLOAT:
      return {DXGI_FORMAT_R32_FLOAT, true};
    case TF::k_32_32_FLOAT:
      return {DXGI_FORMAT_R32G32_FLOAT, true};
    case TF::k_32_32_32_32_FLOAT:
      return {DXGI_FORMAT_R32G32B32A32_FLOAT, true};
    case TF::k_24_8:
    case TF::k_24_8_FLOAT:
      // Depth textures sampled by the game (shadow maps) - bit layout differs
      // from D24S8; handled via resolve targets, not guest memory, for now.
      return {DXGI_FORMAT_R32_UINT, false};
    default:
      return {DXGI_FORMAT_UNKNOWN, false};
  }
}

// Xenos swizzle: 3 bits per destination component, 0-3 = source XYZW, 4 = 0,
// 5 = 1. D3D12 component mapping: 0-3 = source RGBA, 4 = force 0, 5 = force 1.
// For single/dual-component formats the Xenos texture unit replicates the
// last component into the missing ones before swizzling (see xenos.h notes),
// which we emulate by clamping the source index.
uint32_t MapSwizzle(uint32_t swizzle, uint32_t component_count) {
  uint32_t mapping[4];
  for (int i = 0; i < 4; ++i) {
    uint32_t s = (swizzle >> (3 * i)) & 7;
    if (s < 4) {
      s = std::min(s, component_count - 1);
    } else if (s == 4) {
      s = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0;
    } else {
      s = D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1;
    }
    mapping[i] = s;
  }
  return D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(mapping[0], mapping[1], mapping[2], mapping[3]);
}

uint32_t ComponentCount(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
      return 1;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_BC5_UNORM:
      return 2;
    case DXGI_FORMAT_B5G6R5_UNORM:
      return 3;
    default:
      return 4;
  }
}

// Xenos 2D tiled address (port of Xenia's XenosTextureTiledAddress2D from
// texture_address.xesli, the path the GPU texture loads use). Coordinates in
// blocks, result in bytes.
int32_t TiledAddress2D(int32_t x, int32_t y, uint32_t pitch_macro_tiles, uint32_t bpb_log2) {
  int32_t outer_blocks = ((y >> 5) * int32_t(pitch_macro_tiles) + (x >> 5)) << 6;
  int32_t inner_blocks = (((y >> 1) & 0x7) << 3) | (x & 0x7);
  int32_t outer_inner_bytes = (outer_blocks | inner_blocks) << bpb_log2;
  int32_t bank = (y >> 4) & 0x1;
  int32_t pipe = ((x >> 3) & 0x3) ^ (((y >> 3) & 0x1) << 1);
  return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (outer_inner_bytes & 0xF) |
         (((outer_inner_bytes >> 4) & 0x1) << 5) | (((outer_inner_bytes >> 5) & 0x7) << 8) |
         (outer_inner_bytes >> 8 << 12);
}

// Xenos 3D tiled address (port of Xenia's GetTiledOffset3D, reconstructed
// from XGRAPHICS::TileVolume). Coordinates in blocks, result in bytes.
int32_t TiledAddress3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                       uint32_t bpb_log2) {
  pitch = (pitch + 31) & ~31u;
  height = (height + 31) & ~31u;
  int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  int32_t macro = ((((x >> 5) + macro_outer) << (bpb_log2 + 6)) & 0xFFFFFFF) << 1;
  int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bpb_log2 + 6)) >> 6;
  int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                    ((z & 3) << (bpb_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

}  // namespace

bool GetTextureBaseRange(const uint32_t fetch_dwords[6], uint32_t& base, uint32_t& size) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return false;
  TextureInfo info;
  if (!TextureInfo::Prepare(fetch, &info)) return false;
  const FormatInfo* fi = info.format_info();
  TextureExtent src_extent = info.GetMipExtent(0, true);
  uint32_t slice = src_extent.block_pitch_h * src_extent.block_pitch_v * fi->bytes_per_block();
  if (info.is_tiled) slice = (slice + 4095) & ~4095u;
  base = info.memory.base_address;
  size = slice * (info.depth + 1);
  return base && size;
}

bool DecodeTexture(const uint32_t fetch_dwords[6], DecodedTexture& out, const char** reason) {
  auto fail = [&](const char* why) {
    if (reason) *reason = why;
    return false;
  };
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch_dwords, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return fail("not a texture fetch constant");
  }
  TextureInfo info;
  if (!TextureInfo::Prepare(fetch, &info)) {
    return fail("TextureInfo::Prepare failed");
  }
  if (info.dimension != xenos::DataDimension::k2DOrStacked &&
      info.dimension != xenos::DataDimension::kCube &&
      info.dimension != xenos::DataDimension::k3D) {
    return fail("1D texture");
  }
  bool volume = info.dimension == xenos::DataDimension::k3D;
  HostFormat host = MapFormat(info.format);
  if (!host.supported) {
    return fail("unsupported format");
  }
  const FormatInfo* fi = info.format_info();
  const uint32_t bpb = fi->bytes_per_block();
  auto* memory = REX_KERNEL_MEMORY();

  out = DecodedTexture{};
  out.width = info.width + 1;
  out.height = info.height + 1;
  out.depth = info.depth + 1;
  out.dimension = uint32_t(info.dimension);
  out.format = host.format;
  out.guest_format = uint32_t(info.format);
  out.base_address = info.memory.base_address;
  out.component_mapping = MapSwizzle(fetch.swizzle, ComponentCount(host.format));
  // Only levels actually backed by memory.
  uint32_t levels = info.mip_levels();
  if (!info.memory.mip_address || volume) {
    levels = 1;  // 3D: base level only for now
  }
  out.mip_levels = levels;

  auto copy_swap = [endian = info.endianness](void* o, const void* i, size_t len) {
    rex::graphics::texture_conversion::CopySwapBlock(endian, o, i, len);
  };

  for (uint32_t mip = 0; mip < levels; ++mip) {
    uint32_t offset_x = 0, offset_y = 0;
    uint32_t address = info.GetMipLocation(mip, &offset_x, &offset_y, true);
    if (mip == 0 && std::min(out.width, out.height) > 16) {
      // The packed mip tail only holds levels of 16 texels or less; the base
      // level of a larger texture always starts at the base address.
      // (Defensive: TextureInfo::GetMipLocation(0) asks for a packed-tile
      // offset whenever the texture has packed mips.)
      offset_x = offset_y = 0;
    }
    if (!address) {
      out.mip_levels = mip;
      break;
    }
    TextureExtent src_extent = info.GetMipExtent(mip, true);
    TextureExtent dst_extent = info.GetMipExtent(mip, false);
    uint32_t mip_w = std::max(1u, out.width >> mip);
    uint32_t mip_h = std::max(1u, out.height >> mip);
    uint32_t blocks_w = (mip_w + fi->block_width - 1) / fi->block_width;
    uint32_t blocks_h = (mip_h + fi->block_height - 1) / fi->block_height;
    uint32_t dst_pitch = blocks_w * bpb;
    // Slice stride in guest memory (stacked 2D arrays).
    uint32_t src_slice_bytes = src_extent.block_pitch_h * src_extent.block_pitch_v * bpb;
    if (info.is_tiled) {
      // Tiled array slices / cube faces are 4 KB aligned.
      src_slice_bytes = (src_slice_bytes + 4095) & ~4095u;
    }
    if (mip == 0) {
      out.base_size = src_slice_bytes * out.depth;
    }
    if (volume) {
      const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(address);
      uint32_t bpb_log2 = bpb == 16 ? 4 : bpb == 8 ? 3 : bpb == 4 ? 2 : bpb == 2 ? 1 : 0;
      uint32_t pitch_blocks = src_extent.block_pitch_h;
      uint32_t height_blocks = src_extent.block_pitch_v;
      for (uint32_t z = 0; z < out.depth; ++z) {
        DecodedTexture::Level level{mip_w, mip_h, dst_pitch, blocks_h, out.data.size()};
        out.data.resize(out.data.size() + size_t(dst_pitch) * blocks_h);
        uint8_t* dst = out.data.data() + level.offset;
        for (uint32_t y = 0; y < blocks_h; ++y) {
          uint8_t* row = dst + size_t(y) * dst_pitch;
          for (uint32_t x = 0; x < blocks_w; ++x) {
            size_t off = info.is_tiled
                             ? size_t(TiledAddress3D(int32_t(x), int32_t(y), int32_t(z),
                                                     pitch_blocks, height_blocks, bpb_log2))
                             : (size_t(z) * height_blocks + y) * pitch_blocks * bpb + x * bpb;
            copy_swap(row + size_t(x) * bpb, src + off, bpb);
          }
        }
        out.levels.push_back(level);
      }
      continue;
    }
    for (uint32_t slice = 0; slice < out.depth; ++slice) {
      const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(address) +
                           size_t(slice) * src_slice_bytes;
      DecodedTexture::Level level{mip_w, mip_h, dst_pitch, blocks_h, out.data.size()};
      out.data.resize(out.data.size() + size_t(dst_pitch) * blocks_h);
      uint8_t* dst = out.data.data() + level.offset;
      if (!info.is_tiled) {
        uint32_t src_pitch = src_extent.block_pitch_h * bpb;
        const uint8_t* s = src + size_t(offset_y) * src_pitch + size_t(offset_x) * bpb;
        for (uint32_t y = 0; y < blocks_h; ++y) {
          copy_swap(dst + size_t(y) * dst_pitch, s + size_t(y) * src_pitch, dst_pitch);
        }
      } else {
        // Xenos 2D tiling (32x32-block macro tiles), same addressing as the
        // SDK's GPU texture load path.
        uint32_t pitch_blocks = src_extent.block_pitch_h;
        uint32_t bpb_log2 = bpb == 16 ? 4 : bpb == 8 ? 3 : bpb == 4 ? 2 : bpb == 2 ? 1 : 0;
        for (uint32_t y = 0; y < blocks_h; ++y) {
          uint8_t* row = dst + size_t(y) * dst_pitch;
          for (uint32_t x = 0; x < blocks_w; ++x) {
            int32_t off = TiledAddress2D(int32_t(x + offset_x), int32_t(y + offset_y),
                                         (pitch_blocks + 31) >> 5, bpb_log2);
            copy_swap(row + size_t(x) * bpb, src + off, bpb);
          }
        }
      }
      (void)dst_extent;
      out.levels.push_back(level);
    }
  }
  if (out.levels.empty()) {
    return fail("no levels");
  }
  return true;
}

bool WriteDds(const DecodedTexture& tex, const char* path) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  uint32_t header[32] = {};
  header[0] = 124;                           // dwSize
  header[1] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000;  // CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT
  header[2] = tex.height;
  header[3] = tex.width;
  header[6] = tex.mip_levels;
  header[18] = 32;                           // ddspf.dwSize
  header[19] = 0x4;                          // DDPF_FOURCC
  header[20] = '0' << 24 | '1' << 16 | 'X' << 8 | 'D';  // "DX10"
  header[26] = 0x1000;                       // DDSCAPS_TEXTURE
  uint32_t dx10[5] = {uint32_t(tex.format), 3 /*TEXTURE2D*/, 0, tex.depth, 0};
  std::fwrite("DDS ", 1, 4, f);
  std::fwrite(header + 1 - 1, 4, 31, f);
  std::fwrite(dx10, 4, 5, f);
  // DDS order: slice-major, then mips. Our levels are mip-major (slice inner).
  for (uint32_t slice = 0; slice < tex.depth; ++slice) {
    for (uint32_t mip = 0; mip < tex.mip_levels; ++mip) {
      const auto& l = tex.levels[size_t(mip) * tex.depth + slice];
      std::fwrite(tex.data.data() + l.offset, 1, size_t(l.row_pitch) * l.rows, f);
    }
  }
  std::fclose(f);
  return true;
}

}  // namespace conan::native
