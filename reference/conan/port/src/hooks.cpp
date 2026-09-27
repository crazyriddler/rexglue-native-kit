// Mid-ASM hooks for conan. See docs/error_log.md for the evidence behind each
// one (guest address, root cause, why this layer, verification).
//
// These run *inside* recompiled guest code, so they're on the hottest path in
// the whole port: every one is a plain register test with no allocation, no
// logging and no atomics. The diagnostic probe/counter versions these grew
// from were measured at over a million calls per session - see the E041
// entries in docs/error_log.md for what they established before being removed.

#include <rex/ppc.h>

// E016/E017: a family of null-pointer guard hooks. Each site is a place where
// recompiled code dereferences a register without checking it for null, and
// the register is genuinely null on some runs but not others - every instance
// found is only ever reached via an indirect call during the same burst of
// asset-loading activity right after the intro video, and the exact crash
// point varies non-deterministically between otherwise-identical runs.
//
// The guard doesn't invent new behavior: true (register valid) falls through
// to the original instruction unchanged; false (register null) skips it
// exactly the way the function's own logic already skips other "nothing to
// do" cases nearby (each hook's manifest entry names that existing label).
bool conan_nonnull_guard(PPCRegister& reg) {
  return reg.u32 != 0;
}

// E020: a variant where the register already passed an explicit `!= 0` check
// in the guest code itself, yet still held small, clearly-bogus garbage
// (observed: 0x12). The first 64KB of guest address space is never valid for
// a real allocation on this runtime (see the SDK's own `protect_zero` cvar,
// which exists because low/zero-page guest reads are a known-invalid class),
// so reject anything under that rather than only exact zero.
bool conan_plausible_ptr_guard(PPCRegister& reg) {
  return reg.u32 >= 0x10000u;
}

// E018/E026: sites whose stale field isn't null but a code/rodata address
// (observed: 0x7FE3FB78, the raw instruction bytes of an unrelated `mr
// r3,r31` elsewhere in the image; and 0x8257AB00 reaching a Release() call).
// Match the same validity test REX_CALL_INDIRECT_FUNC already uses before
// trusting an indirect-call target (see generated/default/conan_pch.h): the
// candidate must fall within [REX_CODE_BASE, REX_CODE_BASE + REX_CODE_SIZE +
// REX_THUNK_RESERVE_SIZE). These constants are the ones this project's own
// conan_pch.h defines for this image; if the manifest/codegen ever changes
// them, update here too.
bool conan_valid_code_ptr_guard(PPCRegister& reg) {
  constexpr uint32_t kCodeBase = 0x821D0000u;
  constexpr uint32_t kCodeSize = 0x8106BCu;
  constexpr uint32_t kThunkReserve = 0x10000u;
  return (reg.u32 - kCodeBase) < (kCodeSize + kThunkReserve);
}

// EXP-040: idle wait for guest busy-wait loops that only spin on db16cyc
// (Xenon "low priority for 16 cycles" hint, no host code). Time-adaptive per
// wait: spin (pause) for the first 2 ms - the render thread and the job worker
// hand work back and forth many times per frame, and any sleep there costs
// frame time (100 us sleeps made an unlocked frame 6x slower) - then 250 us
// high-resolution sleeps: the long waits are the idle time between frames
// under a frame rate limit, which is where the CPU was burned. A gap of more
// than 50 us between calls means the loop did real work: a new wait starts.
#include <chrono>
#include <immintrin.h>
#include <rex/thread.h>

void conan_spin_idle_wait() {
  using clock = std::chrono::steady_clock;
  thread_local clock::time_point last{}, wait_start{};
  auto now = clock::now();
  if (now - last > std::chrono::microseconds(50)) wait_start = now;
  if (now - wait_start < std::chrono::milliseconds(2)) {
    _mm_pause();
  } else {
    rex::thread::Sleep(std::chrono::microseconds(250));
  }
  last = clock::now();
}
