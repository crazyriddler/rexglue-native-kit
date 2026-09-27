/**
 * @file        core/perf/counter.cpp
 * @brief       Performance counter registry implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/perf/counter.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <intrin.h>
#endif

REXCVAR_DEFINE_STRING(perf_log_csv, "", "Perf",
                      "Path to write per-frame CSV log (empty = disabled)");
REXCVAR_DEFINE_INT32(bench_exit_after_s, 0, "Perf",
                     "Benchmark mode: flush the perf CSV and terminate the process this many "
                     "seconds after the first guest swap (0 = disabled)");

namespace rex::perf {

namespace {

constexpr size_t kNumCounters = static_cast<size_t>(CounterId::kCount);

std::array<std::atomic<int64_t>, kNumCounters> g_counters{};
std::array<std::atomic<int64_t>, kNumCounters> g_snapshot{};

constexpr const char* kCounterNames[] = {
    "frame_time_us",
    "fps",
    "draw_calls",
    "command_buffer_stalls",
    "vertices_processed",
    "xma_frames_decoded",
    "audio_frame_latency_us",
    "buffer_queue_depth",
    "functions_dispatched",
    "interrupt_dispatches",
    "active_threads",
    "apc_queue_depth",
    "critical_region_contentions",
    "texture_cache_hits",
    "texture_cache_misses",
    "pipeline_cache_hits",
    "pipeline_cache_misses",
    "cp_busy_us",
    "cp_idle_us",
    "fence_wait_us",
    "swap_us",
    "gpu_busy_us",
    "submissions",
    "resolves",
    "render_target_switches",
    "barrier_batches",
    "barriers",
    "pipelines_created",
    "shader_translations",
    "draws_skipped_pending",
    "cp_waitregmem_us",
    "cp_waitregmem_count",
    "cp_thread_cpu_us",
    "guest_swap_thread_cpu_us",
    "guest_swap_interval_us",
    "app0",
    "app1",
    "app2",
    "app3",
};
static_assert(std::size(kCounterNames) == kNumCounters, "kCounterNames must match CounterId enum");

// Gauge counters are snapshotted but NOT zeroed each frame.
// Accumulators (everything else) are zeroed after snapshot.
constexpr bool kIsGauge[] = {
    false,  // kFrameTimeUs       (set each frame)
    false,  // kFps               (set each frame)
    false,  // kDrawCalls
    false,  // kCommandBufferStalls
    false,  // kVerticesProcessed
    false,  // kXmaFramesDecoded
    false,  // kAudioFrameLatencyUs
    false,  // kBufferQueueDepth  (set each frame)
    false,  // kFunctionsDispatched
    false,  // kInterruptDispatches
    true,   // kActiveThreads     (inc/dec over lifetime)
    false,  // kApcQueueDepth
    true,   // kCriticalRegionContentions (running total)
    false,  // kTextureCacheHits
    false,  // kTextureCacheMisses
    false,  // kPipelineCacheHits
    false,  // kPipelineCacheMisses
    false,  // kCpBusyUs
    false,  // kCpIdleUs
    false,  // kFenceWaitUs
    false,  // kSwapUs
    false,  // kGpuBusyUs
    false,  // kSubmissions
    false,  // kResolves
    false,  // kRenderTargetSwitches
    false,  // kBarrierBatches
    false,  // kBarriers
    false,  // kPipelinesCreated
    false,  // kShaderTranslations
    false,  // kDrawsSkippedPending
    false,  // kCpWaitRegMemUs
    false,  // kCpWaitRegMemCount
    false,  // kCpThreadCpuUs
    false,  // kGuestSwapThreadCpuUs
    false,  // kGuestSwapIntervalUs
    false,  // kApp0
    false,  // kApp1
    false,  // kApp2
    false,  // kApp3
};
static_assert(std::size(kIsGauge) == kNumCounters, "kIsGauge must match CounterId enum");

// CSV state
std::FILE* g_csv_file = nullptr;
std::string g_csv_path;
int g_csv_frame_count = 0;
std::chrono::steady_clock::time_point g_first_frame{};

}  // anonymous namespace

const char* CounterName(CounterId id) {
  auto idx = static_cast<size_t>(id);
  if (idx < kNumCounters)
    return kCounterNames[idx];
  return "unknown";
}

void SetCounter(CounterId id, int64_t value) {
  g_counters[static_cast<size_t>(id)].store(value, std::memory_order_relaxed);
}

void IncrementCounter(CounterId id, int64_t delta) {
  g_counters[static_cast<size_t>(id)].fetch_add(delta, std::memory_order_relaxed);
}

int64_t GetCounter(CounterId id) {
  return g_counters[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void ResetFrameCounters() {
  for (size_t i = 0; i < kNumCounters; ++i) {
    if (kIsGauge[i]) {
      // Gauges: snapshot the current value, don't zero
      g_snapshot[i].store(g_counters[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    } else {
      // Accumulators: snapshot and zero for next frame
      g_snapshot[i].store(g_counters[i].exchange(0, std::memory_order_relaxed),
                          std::memory_order_relaxed);
    }
  }
}

int64_t GetSnapshotCounter(CounterId id) {
  return g_snapshot[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

void Init() {
  for (auto& c : g_counters)
    c.store(0, std::memory_order_relaxed);
  for (auto& s : g_snapshot)
    s.store(0, std::memory_order_relaxed);
}

void SetCsvLogPath(const std::string& path) {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path = path;
  g_csv_frame_count = 0;

  if (path.empty())
    return;

  g_csv_file = rex::filesystem::OpenFile(rex::to_path(path), "w");
  if (!g_csv_file) {
    REXLOG_WARN("perf: failed to open CSV log: {}", path);
    g_csv_path.clear();
    return;
  }

  // Write header. t_ms = wall-clock ms since the first guest swap.
  std::fputs("t_ms", g_csv_file);
  for (size_t i = 0; i < kNumCounters; ++i) {
    std::fputc(',', g_csv_file);
    std::fputs(kCounterNames[i], g_csv_file);
  }
  std::fputc('\n', g_csv_file);
}

int64_t SampleThreadCpuUs(int slot) {
#if defined(_WIN32)
  // TSC ticks per microsecond, calibrated once against QPC.
  static const double tsc_per_us = [] {
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    uint64_t c0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&q1);
    uint64_t c1 = __rdtsc();
    double us = double(q1.QuadPart - q0.QuadPart) * 1e6 / double(f.QuadPart);
    return double(c1 - c0) / us;
  }();
  thread_local uint64_t last[8] = {};
  ULONG64 cycles = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  uint64_t prev = last[slot & 7];
  last[slot & 7] = cycles;
  if (!prev) {
    return 0;
  }
  return int64_t(double(cycles - prev) / tsc_per_us);
#else
  (void)slot;
  return 0;
#endif
}

namespace {
std::mutex g_exit_callbacks_mutex;
std::vector<void (*)()> g_exit_callbacks;
}  // namespace

namespace {
std::atomic<void (*)(uint64_t, const uint32_t*, uint32_t)> g_gpu_swap_callback{nullptr};
}  // namespace

void RegisterGpuSwapCallback(void (*callback)(uint64_t swap_number, const uint32_t* regs,
                                              uint32_t count)) {
  g_gpu_swap_callback.store(callback);
}

void NotifyGpuSwap(uint64_t swap_number, const uint32_t* regs, uint32_t count) {
  if (auto* callback = g_gpu_swap_callback.load()) callback(swap_number, regs, count);
}

void RegisterBenchExitCallback(void (*callback)()) {
  std::lock_guard<std::mutex> lock(g_exit_callbacks_mutex);
  g_exit_callbacks.push_back(callback);
}

double BenchElapsedMs() {
  if (g_first_frame.time_since_epoch().count() == 0) {
    return 0.0;
  }
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                   g_first_frame)
      .count();
}

void WriteCsvFrame() {
  // Lazily honour the --perf_log_csv cvar. Nothing else in the tree ever calls
  // SetCsvLogPath(), so without this the cvar was inert and the whole per-frame
  // counter CSV path was dead code. Doing it here (rather than at startup) keeps
  // it independent of module init order: Profiler::Flip() is driven by the GPU
  // worker thread, by which point command-line cvars are long since parsed.
  static bool cvar_checked = false;
  if (!cvar_checked) {
    cvar_checked = true;
    const std::string& path = REXCVAR_GET(perf_log_csv);
    if (!path.empty() && !g_csv_file) {
      SetCsvLogPath(path);
      if (g_csv_file) {
        REXLOG_INFO("perf: per-frame counter CSV -> {}", path);
      }
    }
  }

  auto now = std::chrono::steady_clock::now();
  if (g_first_frame.time_since_epoch().count() == 0) {
    g_first_frame = now;
  }
  double t_ms = std::chrono::duration<double, std::milli>(now - g_first_frame).count();

  int32_t exit_after_s = REXCVAR_GET(bench_exit_after_s);
  if (exit_after_s > 0 && t_ms >= exit_after_s * 1000.0) {
    REXLOG_INFO("perf: bench_exit_after_s={} reached, terminating", exit_after_s);
    FlushCsv();
    DumpSampleProfile();
    {
      std::lock_guard<std::mutex> lock(g_exit_callbacks_mutex);
      for (auto* callback : g_exit_callbacks) {
        callback();
      }
    }
    rex::FlushLogging();
#if defined(_WIN32)
    ::TerminateProcess(::GetCurrentProcess(), 0);
#endif
    std::_Exit(0);
  }

  if (!g_csv_file)
    return;

  std::fprintf(g_csv_file, "%.3f", t_ms);
  for (size_t i = 0; i < kNumCounters; ++i) {
    std::fputc(',', g_csv_file);
    std::fprintf(g_csv_file, "%lld",
                 static_cast<long long>(g_snapshot[i].load(std::memory_order_relaxed)));
  }
  std::fputc('\n', g_csv_file);

  if (++g_csv_frame_count % 60 == 0) {
    std::fflush(g_csv_file);
  }
}

void FlushCsv() {
  if (g_csv_file) {
    std::fflush(g_csv_file);
    std::fclose(g_csv_file);
    g_csv_file = nullptr;
  }
  g_csv_path.clear();
}

}  // namespace rex::perf
