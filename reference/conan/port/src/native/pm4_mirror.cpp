#include "pm4_mirror.h"

namespace conan::native {

namespace {

inline uint32_t LoadBE(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

// 0xA0000000 is the 1:1 cached view of guest physical memory.
constexpr uint32_t kPhysicalView = 0xA0000000u;

}  // namespace

void Pm4Mirror::ScanFrom(const uint8_t* base, const uint8_t* stream, uint32_t begin,
                         uint32_t end, int depth) {
  uint32_t p = begin;
  while (p + 4 <= end) {
    uint32_t header = LoadBE(stream, p);
    p += 4;
    ++packets;
    switch (header >> 30) {
      case 0: {
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        uint32_t index = header & 0x7FFF;
        bool one_register = (header >> 15) & 1;
        if (p + count * 4 > end) return;
        for (uint32_t i = 0; i < count; ++i) {
          Write(one_register ? index : index + i, LoadBE(stream, p + 4 * i));
        }
        p += count * 4;
        dwords += count;
        break;
      }
      case 1: {
        if (p + 8 > end) return;
        Write(header & 0x7FF, LoadBE(stream, p));
        Write((header >> 11) & 0x7FF, LoadBE(stream, p + 4));
        p += 8;
        break;
      }
      case 2:
        break;
      case 3: {
        uint32_t opcode = (header >> 8) & 0x7F;
        uint32_t count = ((header >> 16) & 0x3FFF) + 1;
        if (p + count * 4 > end) return;
        uint32_t d0 = count > 0 ? LoadBE(stream, p) : 0;
        switch (opcode) {
          case 0x2D: {  // SET_CONSTANT
            uint32_t index = d0 & 0x7FF;
            uint32_t first;
            switch ((d0 >> 16) & 0xFF) {
              case 0: first = kAluConstantBase + index; break;
              case 1: first = kFetchConstantBase + index; break;
              case 2: first = kBoolConstantBase + index; break;
              case 3: first = kLoopConstantBase + index; break;
              case 4: first = 0x2000 + index; break;
              default: first = kRegisterCount; break;
            }
            for (uint32_t i = 1; i < count; ++i) Write(first + i - 1, LoadBE(stream, p + 4 * i));
            break;
          }
          case 0x55:    // SET_CONSTANT2
          case 0x56: {  // SET_SHADER_CONSTANTS
            uint32_t index = d0 & 0xFFFF;
            for (uint32_t i = 1; i < count; ++i) Write(index + i - 1, LoadBE(stream, p + 4 * i));
            break;
          }
          case 0x2F: {  // LOAD_ALU_CONSTANT (from memory)
            if (count < 3) break;
            uint32_t address = d0 & 0x3FFFFFFF;
            uint32_t offset_type = LoadBE(stream, p + 4);
            uint32_t size = LoadBE(stream, p + 8) & 0xFFF;
            uint32_t index = offset_type & 0x7FF;
            uint32_t first;
            switch ((offset_type >> 16) & 0xFF) {
              case 0: first = kAluConstantBase + index; break;
              case 1: first = kFetchConstantBase + index; break;
              case 2: first = kBoolConstantBase + index; break;
              case 3: first = kLoopConstantBase + index; break;
              case 4: first = 0x2000 + index; break;
              default: first = kRegisterCount; break;
            }
            uint32_t src = kPhysicalView + (address & 0x1FFFFFFF);
            for (uint32_t i = 0; i < size; ++i) Write(first + i, LoadBE(base, src + 4 * i));
            break;
          }
          case 0x22:    // DRAW_INDX
          case 0x36: {  // DRAW_INDX_2
            if ((regs_[0x2208] & 7) == 6) {  // RB_MODECONTROL edram_mode = copy
              last_copy_dest_info = regs_[0x231B];
              last_copy_dest_base = regs_[0x2319];
              ++copy_draws;
            }
            break;
          }
          case 0x3F:    // INDIRECT_BUFFER
          case 0x37: {  // INDIRECT_BUFFER_PFD
            ++indirect_buffers;
            if (count < 2 || depth > 2 || !follow_indirect) break;
            uint32_t address = kPhysicalView + (d0 & 0x1FFFFFFF);
            uint32_t size = LoadBE(stream, p + 4) & 0xFFFFF;
            if (size > (1u << 18)) break;  // not a plausible command buffer
            indirect_dwords += size;
            ScanFrom(base, base, address, address + size * 4, depth + 1);
            break;
          }
          default:
            break;
        }
        p += count * 4;
        dwords += count;
        break;
      }
    }
  }
}

}  // namespace conan::native
