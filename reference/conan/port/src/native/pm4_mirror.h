// Mirror of the Xenos register state as the GPU will see it, rebuilt from the
// PM4 packets the XDK writes into its command segments.
//
// The XDK device shadow (dev+0x780 constants, dev+0x480 fetch constants) does
// not see every write: shader literal constants (LOAD_ALU_CONSTANT at shader
// bind), GpuBeginShaderConstantF4 / inline SET_CONSTANT packets and inline
// type-0 constant blocks (SpeedTree tables, per-object lights) go straight to
// the command buffer. Parsing the packets between draws gives the exact state.
#pragma once

#include <cstdint>
#include <cstring>

namespace conan::native {

class Pm4Mirror {
 public:
  static constexpr uint32_t kRegisterCount = 0x5003;
  static constexpr uint32_t kAluConstantBase = 0x4000;  // VS c0-255, PS c256-511 (x4 dwords)
  static constexpr uint32_t kFetchConstantBase = 0x4800;
  static constexpr uint32_t kBoolConstantBase = 0x4900;
  static constexpr uint32_t kLoopConstantBase = 0x4908;

  // Parses the command stream between guest virtual addresses [begin, end).
  void Scan(const uint8_t* base, uint32_t begin, uint32_t end, int depth = 0) {
    ScanFrom(base, base, begin, end, depth);
  }
  // Parses a copy of a command segment (`size` bytes at `words`, big-endian);
  // packets that reference memory (LOAD_ALU_CONSTANT, INDIRECT_BUFFER) read
  // guest memory through `base`.
  void ScanCopy(const uint8_t* base, const uint8_t* words, uint32_t size) {
    ScanFrom(base, words, 0, size, 0);
  }

  uint32_t reg(uint32_t index) const { return index < kRegisterCount ? regs_[index] : 0; }
  const uint32_t* regs() const { return regs_; }
  // Registers written at least once since startup (0x4000-0x4A00 range tracked).
  bool written(uint32_t index) const {
    return index >= kAluConstantBase && index < kAluConstantBase + kTrackedCount &&
           written_[index - kAluConstantBase];
  }

  uint64_t packets = 0;
  uint64_t dwords = 0;
  uint64_t unknown_packets = 0;
  uint64_t indirect_buffers = 0;
  bool follow_indirect = true;
  // RB_COPY_* state captured at the last resolve (copy-mode draw); the XDK
  // resets these registers right after the copy.
  uint32_t last_copy_dest_info = 0;
  uint32_t last_copy_dest_base = 0;
  uint64_t copy_draws = 0;
  // Bumped on any VS / PS float constant write (constant buffer reuse).
  uint64_t vs_version = 1, ps_version = 1;
  uint64_t indirect_dwords = 0;

 private:
  // Main stream read from `stream` + address; memory references via `base`.
  void ScanFrom(const uint8_t* base, const uint8_t* stream, uint32_t begin, uint32_t end,
                int depth);
  static constexpr uint32_t kTrackedCount = 0xA00;
  void Write(uint32_t index, uint32_t value) {
    if (index >= kRegisterCount) return;
    if (index - kAluConstantBase < 0x800u) {
      ++(index < kAluConstantBase + 0x400 ? vs_version : ps_version);
    }
    regs_[index] = value;
    if (index >= kAluConstantBase && index < kAluConstantBase + kTrackedCount) {
      written_[index - kAluConstantBase] = true;
    }
  }
  uint32_t regs_[kRegisterCount] = {};
  bool written_[kTrackedCount] = {};
};

}  // namespace conan::native
