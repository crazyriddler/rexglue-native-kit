/**
 * @file        core/perf/sampler.cpp
 * @brief       In-process sampling profiler for benchmark runs.
 *
 * Threads opt in with RegisterSampledThread(slot, name). A background thread
 * suspends each registered thread at ~2 kHz, captures RIP and a short unwound
 * call stack, and aggregates exclusive (RIP) and inclusive (function start)
 * hit counts. DumpSampleProfile() writes them as module+RVA text for offline
 * symbolization with llvm-symbolizer (tools/symbolize_profile.py).
 *
 * Nothing is allocated while a target thread is suspended.
 */
#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <map>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <unordered_set>
#endif

REXCVAR_DEFINE_STRING(sample_profile_out, "", "Perf",
                      "Benchmark: write a sampling profile of registered threads (guest swap "
                      "thread, GPU command processor) to this path at bench exit");
REXCVAR_DEFINE_DOUBLE(sample_profile_start_s, 50.0, "Perf",
                      "Benchmark: seconds since the first guest swap at which sampling starts");
REXCVAR_DEFINE_BOOL(sample_profile_all_threads, false, "Perf",
                    "Benchmark: also sample every other thread of the process (slots 8+, named "
                    "after the thread description) - CPU-usage investigations");
REXCVAR_DEFINE_INT32(sample_profile_dump_interval_s, 0, "Perf",
                     "Also rewrite the sample profile every N seconds (hang diagnosis; 0 = only "
                     "at bench exit)");

namespace rex::perf {

#if defined(_WIN32)
namespace {

constexpr int kMaxSlots = 64;
constexpr int kFirstAutoSlot = 8;  // sample_profile_all_threads
constexpr int kMaxFrames = 24;

struct SlotState {
  std::atomic<HANDLE> handle{nullptr};
  std::string name;
  uint64_t samples = 0;
  std::unordered_map<uint64_t, uint64_t> exclusive;
  std::unordered_map<uint64_t, uint64_t> inclusive;
  // Full call stacks (return addresses, leaf first) for caller/callee analysis.
  std::map<std::vector<uint64_t>, uint64_t> stacks;
};

SlotState g_slots[kMaxSlots];
std::mutex g_mutex;
std::atomic<bool> g_started{false};
std::atomic<bool> g_stop{false};

void DumpLocked();

// sample_profile_all_threads: registers threads not seen yet into free slots.
void RegisterAllThreads(std::unordered_set<DWORD>& known) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 e{};
  e.dwSize = sizeof(e);
  DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
  for (BOOL ok = Thread32First(snap, &e); ok; ok = Thread32Next(snap, &e)) {
    if (e.th32OwnerProcessID != pid || e.th32ThreadID == self || known.count(e.th32ThreadID)) {
      continue;
    }
    known.insert(e.th32ThreadID);
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                              THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, e.th32ThreadID);
    if (!h) continue;
    // Skip threads already registered explicitly.
    bool explicit_slot = false;
    for (int slot = 0; slot < kFirstAutoSlot; ++slot) {
      HANDLE other = g_slots[slot].handle.load();
      explicit_slot |= other && GetThreadId(other) == e.th32ThreadID;
    }
    int free_slot = -1;
    for (int slot = kFirstAutoSlot; slot < kMaxSlots && free_slot < 0; ++slot) {
      if (!g_slots[slot].handle.load()) free_slot = slot;
    }
    if (explicit_slot || free_slot < 0) {
      CloseHandle(h);
      continue;
    }
    std::string name = std::to_string(e.th32ThreadID);
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      char narrow[256] = {};
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
      if (narrow[0]) name += std::string(" ") + narrow;
      LocalFree(desc);
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_slots[free_slot].name = name;
    }
    g_slots[free_slot].handle.store(h, std::memory_order_release);
  }
  CloseHandle(snap);
}

void SamplerThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  auto last_dump = std::chrono::steady_clock::now();
  uint64_t stack[kMaxFrames];
  uint64_t funcs[kMaxFrames];
  std::unordered_set<DWORD> known_threads;
  auto last_enum = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  while (!g_stop.load(std::memory_order_relaxed)) {
    Sleep(0);
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    if (BenchElapsedMs() < REXCVAR_GET(sample_profile_start_s) * 1000.0) {
      continue;
    }
    if (REXCVAR_GET(sample_profile_all_threads) &&
        std::chrono::steady_clock::now() - last_enum > std::chrono::seconds(1)) {
      last_enum = std::chrono::steady_clock::now();
      RegisterAllThreads(known_threads);
    }
    if (int32_t interval = REXCVAR_GET(sample_profile_dump_interval_s); interval > 0) {
      auto now = std::chrono::steady_clock::now();
      if (now - last_dump > std::chrono::seconds(interval)) {
        last_dump = now;
        std::lock_guard<std::mutex> lock(g_mutex);
        DumpLocked();
        // Each periodic dump covers only the last interval.
        for (SlotState& st : g_slots) {
          st.samples = 0;
          st.exclusive.clear();
          st.inclusive.clear();
          st.stacks.clear();
        }
      }
    }
    for (int slot = 0; slot < kMaxSlots; ++slot) {
      HANDLE h = g_slots[slot].handle.load(std::memory_order_acquire);
      if (!h) {
        continue;
      }
      if (SuspendThread(h) == DWORD(-1)) {
        continue;
      }
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      int depth = 0;
      int nfuncs = 0;
      if (GetThreadContext(h, &ctx)) {
        // Unwind while suspended; no allocations here.
        for (depth = 0; depth < kMaxFrames && ctx.Rip; ++depth) {
          stack[depth] = ctx.Rip;
          DWORD64 image_base = 0;
          PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
          if (!fn) {
            // Leaf without unwind info: return address at [rsp].
            funcs[nfuncs++] = ctx.Rip;
            if (ctx.Rsp == 0) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
          }
          funcs[nfuncs++] = image_base + fn->BeginAddress;
          void* handler_data = nullptr;
          DWORD64 establisher = 0;
          RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data,
                           &establisher, nullptr);
        }
      }
      ResumeThread(h);
      if (!depth) {
        continue;
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      SlotState& s = g_slots[slot];
      ++s.samples;
      ++s.exclusive[stack[0]];
      ++s.stacks[std::vector<uint64_t>(stack, stack + depth)];
      // Inclusive: count each distinct function once per sample.
      for (int i = 0; i < nfuncs; ++i) {
        bool dup = false;
        for (int j = 0; j < i; ++j) {
          if (funcs[j] == funcs[i]) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          ++s.inclusive[funcs[i]];
        }
      }
    }
  }
}

void WriteAddr(std::FILE* f, const char* kind, int slot, uint64_t addr, uint64_t count) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(addr), &module)) {
    GetModuleFileNameA(module, path, MAX_PATH);
  }
  std::fprintf(f, "%s\t%d\t%llu\t%s\t0x%llx\n", kind, slot, (unsigned long long)count, path,
               (unsigned long long)(addr - reinterpret_cast<uint64_t>(module)));
}

}  // namespace

void RegisterSampledThread(int slot, const char* name) {
  if (REXCVAR_GET(sample_profile_out).empty() || slot < 0 || slot >= kMaxSlots) {
    return;
  }
  if (g_slots[slot].handle.load()) {
    return;
  }
  HANDLE h = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_slots[slot].name = name;
  }
  g_slots[slot].handle.store(h, std::memory_order_release);
  bool expected = false;
  if (g_started.compare_exchange_strong(expected, true)) {
    std::thread(SamplerThread).detach();
  }
}

void DumpSampleProfile() {
  if (REXCVAR_GET(sample_profile_out).empty() || !g_started.load()) {
    return;
  }
  g_stop.store(true);
  std::lock_guard<std::mutex> lock(g_mutex);
  DumpLocked();
}

namespace {
void DumpLocked() {
  const std::string& path = REXCVAR_GET(sample_profile_out);
  std::FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    return;
  }
  for (int slot = 0; slot < kMaxSlots; ++slot) {
    const SlotState& s = g_slots[slot];
    if (!s.samples) {
      continue;
    }
    std::fprintf(f, "slot\t%d\t%llu\t%s\n", slot, (unsigned long long)s.samples, s.name.c_str());
    for (auto& [addr, count] : s.exclusive) {
      WriteAddr(f, "excl", slot, addr, count);
    }
    for (auto& [addr, count] : s.inclusive) {
      WriteAddr(f, "incl", slot, addr, count);
    }
    for (auto& [frames, count] : s.stacks) {
      std::fprintf(f, "stack\t%d\t%llu\t", slot, (unsigned long long)count);
      for (size_t i = 0; i < frames.size(); ++i) {
        std::fprintf(f, i ? ";%llx" : "%llx", (unsigned long long)frames[i]);
      }
      std::fputc('\n', f);
    }
  }
  // Loaded modules (absolute address ranges) to symbolize the stacks.
  HMODULE mods[512];
  DWORD needed = 0;
  if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 512; ++i) {
      MODULEINFO mi{};
      char path[MAX_PATH] = "?";
      GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi));
      GetModuleFileNameA(mods[i], path, MAX_PATH);
      std::fprintf(f, "module\t%llx\t%lx\t%s\n", (unsigned long long)mi.lpBaseOfDll,
                   (unsigned long)mi.SizeOfImage, path);
    }
  }
  std::fclose(f);
}
}  // namespace

#else

void RegisterSampledThread(int, const char*) {}
void DumpSampleProfile() {}

#endif

}  // namespace rex::perf
