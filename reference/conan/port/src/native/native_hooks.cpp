// Hooks that route the game's XDK D3D calls into the native renderer when
// --native_renderer is enabled. With the flag off every hook is a plain
// pass-through to the recompiled original.

#include <cstring>

#include <rex/ppc.h>

#include "hang_watchdog.h"
#include "native_graphics_system.h"
#include "native_renderer.h"

// D3DDevice_Swap(dev, pFrontBuffer, pParameters) - the only VdSwap caller.
// The original still runs so the guest's swap/vblank/fence semantics are
// untouched (Xenos consumes the packets in sync-only mode); the native frame
// is presented right after.
REX_EXTERN(__imp__sub_822E8EB8);
extern "C" REX_FUNC(sub_822E8EB8) {
  uint32_t front_buffer = ctx.r4.u32;
  // Guest swap number (1 = first swap), same numbering as the Xenos CP's
  // bench_screenshot_swaps for frame-exact A/B comparisons.
  static uint64_t swap_number = 0;
  ++swap_number;
  conan::native::HangWatchdogBeat();
  __imp__sub_822E8EB8(ctx, base);
  if (conan::native::Enabled()) {
    conan::native::Renderer::Get().OnSwap(base, front_buffer, swap_number);
  }
}

// XDK GPU waits. BlockOnFence (sub_822DF1D8: r3 = device, r4 = fence) loops
// calling the poll sub_822DE050 (a few pause instructions, a read-pointer check
// and the 5 s hang detector; returns 1 while the caller should keep waiting)
// until the GPU's fence counter passes `fence`:
//   (dev[10908] - fence) >= (dev[10908] - *dev[10896])   (unsigned)
// On the console that spin is free; on a PC it burned a whole core at any frame
// rate limit (the GPU side waits for vblank). The poll hook sleeps instead:
// inside BlockOnFence until that exact fence condition holds, elsewhere until
// any GPU progress (1 ms cap either way, so the hang detector still runs).
namespace {
thread_local uint32_t t_fence_dev = 0, t_fence_value = 0;

inline uint32_t GuestLoad32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
}  // namespace

REX_EXTERN(__imp__sub_822DF1D8);
extern "C" REX_FUNC(sub_822DF1D8) {
  uint32_t saved_dev = t_fence_dev, saved_value = t_fence_value;
  t_fence_dev = ctx.r3.u32;
  t_fence_value = ctx.r4.u32;
  __imp__sub_822DF1D8(ctx, base);
  t_fence_dev = saved_dev;
  t_fence_value = saved_value;
}

REX_EXTERN(__imp__sub_822DE050);
extern "C" REX_FUNC(sub_822DE050) {
  uint64_t generation = conan::native::GpuProgressGeneration();
  __imp__sub_822DE050(ctx, base);
  if (ctx.r3.u32 != 1) return;
  if (t_fence_dev) {
    uint32_t dev = t_fence_dev, fence = t_fence_value;
    conan::native::WaitForGpuCondition(
        [base, dev, fence] {
          uint32_t current = GuestLoad32(base, dev + 10908);
          uint32_t completed = GuestLoad32(base, GuestLoad32(base, dev + 10896));
          return current - fence >= current - completed;
        },
        1000);
  } else {
    conan::native::WaitForGpuProgress(generation, 1000);
  }
}

// EXP-047: sub_824E7678(obj, name) - the engine's parameter-by-name lookup: a
// linear strcmp scan of two null-separated name tables of `obj` (+264/+648
// and +268/+656, counts +272/+276), returning a handle built from the match
// position (0 if absent). Pure (no stores), called many times per frame from
// the render thread: ~5% of it. Results are cached per thread, keyed by the
// object, its table pointers/counts and the name's contents, so a replaced
// object or table never hits a stale entry.
#include <unordered_map>
#include <rex/hash.h>

REX_EXTERN(__imp__sub_824E7678);
extern "C" REX_FUNC(sub_824E7678) {
  uint32_t obj = ctx.r3.u32, name = ctx.r4.u32;
  auto load = [&](uint32_t address) {
    uint32_t v;
    std::memcpy(&v, base + address, 4);
    return __builtin_bswap32(v);
  };
  // Name (bounded; longer names bypass the cache).
  const char* str = reinterpret_cast<const char*>(base + name);
  size_t len = 0;
  while (len < 128 && str[len]) ++len;
  if (len >= 128) {
    __imp__sub_824E7678(ctx, base);
    return;
  }
  struct Key {
    uint32_t obj, t0, t1, n0, n1, s0, s1, pad;  // no implicit padding: hashed as bytes
    uint64_t name_hash;
    bool operator==(const Key&) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return size_t(XXH3_64bits(&k, sizeof(k)));
    }
  };
  Key key{obj,           load(obj + 264), load(obj + 268), load(obj + 272), load(obj + 276),
          load(obj + 648), load(obj + 656), 0,               XXH3_64bits(str, len)};
  thread_local std::unordered_map<Key, uint32_t, KeyHash> cache;
  auto it = cache.find(key);
  if (it != cache.end()) {
    ctx.r3.u64 = it->second;
    return;
  }
  __imp__sub_824E7678(ctx, base);
  if (cache.size() > 65536) cache.clear();
  cache.emplace(key, ctx.r3.u32);
}
