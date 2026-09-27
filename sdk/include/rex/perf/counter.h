/**
 * @file        perf/counter.h
 * @brief       Performance counter registry and profiler
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#endif

namespace rex::perf {

enum class CounterId : uint16_t {
  // Frame
  kFrameTimeUs,
  kFps,

  // GPU
  kDrawCalls,
  kCommandBufferStalls,
  kVerticesProcessed,

  // Audio
  kXmaFramesDecoded,
  kAudioFrameLatencyUs,
  kBufferQueueDepth,

  // Dispatch
  kFunctionsDispatched,
  kInterruptDispatches,

  // Threading
  kActiveThreads,
  kApcQueueDepth,
  kCriticalRegionContentions,

  // Caches
  kTextureCacheHits,
  kTextureCacheMisses,
  kPipelineCacheHits,
  kPipelineCacheMisses,

  // --- M1 baseline instrumentation (native-renderer migration mission) ---
  // Frame time decomposition, all microseconds, accumulated per frame.
  kCpBusyUs,     // GPU worker thread inside ExecutePrimaryBuffer (PM4 translation)
  kCpIdleUs,     // GPU worker thread starved: no guest commands available
  kFenceWaitUs,  // GPU worker thread blocked on a D3D12 fence (GPU/sync bound)
  kSwapUs,       // time inside IssueSwap (present + swapchain work)
  kGpuBusyUs,    // GPU-side timestamp delta summed over the frame's submissions

  // GPU work shape.
  kSubmissions,           // ExecuteCommandLists calls
  kResolves,              // IssueCopy (EDRAM resolve) invocations
  kRenderTargetSwitches,  // OMSetRenderTargets issued by the render target cache
  kBarrierBatches,        // ResourceBarrier calls
  kBarriers,              // individual barriers submitted
  kPipelinesCreated,      // host PSO creations
  kShaderTranslations,    // guest shader -> DXBC translations
  kDrawsSkippedPending,   // draws dropped because their pipeline is still being created
  kCpWaitRegMemUs,        // CP thread blocked in WAIT_REG_MEM (waiting on guest CPU/memory)
  kCpWaitRegMemCount,     // WAIT_REG_MEM packets that actually had to wait
  kCpThreadCpuUs,         // CPU time actually consumed by the CP thread this frame
  kGuestSwapThreadCpuUs,  // CPU time consumed by the guest thread calling VdSwap, per its swap
  kGuestSwapIntervalUs,   // wall time between consecutive guest VdSwap calls
  kApp0,                  // free for app/game-side instrumentation (CSV: app0..app3)
  kApp1,
  kApp2,
  kApp3,

  kCount  // sentinel -- must be last
};

// Returns human-readable name for a counter (e.g. "frame_time_us")
const char* CounterName(CounterId id);

// Set a counter to an absolute value
void SetCounter(CounterId id, int64_t value);

// Atomically add to a counter
void IncrementCounter(CounterId id, int64_t delta = 1);

// Read a counter's current live value
int64_t GetCounter(CounterId id);

// Snapshot current values into the read buffer and zero the live counters.
// Called once per frame by Profiler::Flip().
void ResetFrameCounters();

// Read a counter from the last-frame snapshot (stable between frames).
int64_t GetSnapshotCounter(CounterId id);

// Initialize the counter system (zeroes everything). Safe to call multiple times.
void Init();

// CSV logging
void SetCsvLogPath(const std::string& path);
void WriteCsvFrame();
void FlushCsv();

// Wall-clock milliseconds since the first guest swap (benchmark time base;
// same clock as the CSV's t_ms column). 0 before the first swap.
double BenchElapsedMs();

// CPU time (microseconds) consumed by the calling thread since the previous
// call with the same slot from that thread. Uses QueryThreadCycleTime,
// calibrated against the TSC. Slot must be < 8.
int64_t SampleThreadCpuUs(int slot);

// Sampling profiler (src/core/perf/sampler.cpp), active when the
// sample_profile_out cvar is set. Call from the thread to be sampled.
void RegisterSampledThread(int slot, const char* name);
void DumpSampleProfile();

// Callbacks run (on the GPU thread) right before bench_exit_after_s terminates
// the process - for app-side captures that need to be flushed.
void RegisterBenchExitCallback(void (*callback)());

// Called by the GPU command processor after it processes each guest swap
// (swap_number 1 = first swap), e.g. for frame-exact A/B dumps.
// regs: the command processor's register file at that point (count entries).
void RegisterGpuSwapCallback(void (*callback)(uint64_t swap_number, const uint32_t* regs,
                                              uint32_t count));
void NotifyGpuSwap(uint64_t swap_number, const uint32_t* regs, uint32_t count);

// RAII helper: accumulates the wall-clock duration of its scope into a counter.
// Used by the M1 baseline instrumentation to decompose the GPU worker thread's
// frame time into busy / idle / fence-wait buckets.
class ScopedMicrosecondAccumulator {
 public:
  explicit ScopedMicrosecondAccumulator(CounterId id)
      : id_(id), start_(std::chrono::steady_clock::now()) {}
  ~ScopedMicrosecondAccumulator() {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - start_)
                       .count();
    IncrementCounter(id_, static_cast<int64_t>(elapsed));
  }
  ScopedMicrosecondAccumulator(const ScopedMicrosecondAccumulator&) = delete;
  ScopedMicrosecondAccumulator& operator=(const ScopedMicrosecondAccumulator&) = delete;

 private:
  CounterId id_;
  std::chrono::steady_clock::time_point start_;
};

// Profiler -- coordinates Tracy frame marks and counter snapshots.
// Moved here from rex::debug to consolidate all perf code under rex::perf.
class Profiler {
 public:
  static void Startup() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (!tracy::IsProfilerStarted())
      tracy::StartupProfiler();
#endif
  }
  static void OnThreadEnter(const char* name = nullptr) {
#ifdef REXGLUE_ENABLE_PROFILING
    if (name && tracy::IsProfilerStarted())
      tracy::SetThreadName(name);
#else
    (void)name;
#endif
  }
  static void OnThreadExit() {}
  static void ThreadEnter(const char* name = nullptr) { OnThreadEnter(name); }
  static void ThreadExit() {}
  static void Flip() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted()) {
      FrameMark;
    }
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ResetFrameCounters();
    WriteCsvFrame();
#endif
  }
  static void Flush() {}
  static void Shutdown() {
#ifdef REXGLUE_ENABLE_PROFILING
    if (tracy::IsProfilerStarted())
      tracy::ShutdownProfiler();
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    FlushCsv();
#endif
  }
  static bool is_enabled() {
#ifdef REXGLUE_ENABLE_PROFILING
    return tracy::IsProfilerStarted();
#else
    return false;
#endif
  }
};

}  // namespace rex::perf

// Perf counter macros -- compile to no-ops when counters are disabled.
#ifdef REXGLUE_ENABLE_PERF_COUNTERS

// Generic helpers for easily adding new counters
#define PERF_counter_set(id, value) rex::perf::SetCounter(rex::perf::CounterId::id, value)
#define PERF_counter_inc(id) rex::perf::IncrementCounter(rex::perf::CounterId::id)
#define PERF_counter_add(id, delta) rex::perf::IncrementCounter(rex::perf::CounterId::id, delta)

// Purpose-specific macros so callsites stay clean
#define PROFILE_FRAME_TIME_US(value) PERF_counter_set(kFrameTimeUs, value)
#define PROFILE_FPS(value) PERF_counter_set(kFps, value)
#define PROFILE_FUNCTION_DISPATCHED() PERF_counter_inc(kFunctionsDispatched)
#define PROFILE_INTERRUPT_DISPATCHED() PERF_counter_inc(kInterruptDispatches)
#define PROFILE_XMA_FRAME_DECODED() PERF_counter_inc(kXmaFramesDecoded)
#define PROFILE_DRAW_CALL() PERF_counter_inc(kDrawCalls)
#define PROFILE_VERTICES(n) PERF_counter_add(kVerticesProcessed, n)
#define PROFILE_CMD_BUFFER_STALL() PERF_counter_inc(kCommandBufferStalls)
#define PROFILE_AUDIO_LATENCY_US(value) PERF_counter_set(kAudioFrameLatencyUs, value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value) PERF_counter_set(kBufferQueueDepth, value)
#define PROFILE_THREAD_CREATED() PERF_counter_inc(kActiveThreads)
#define PROFILE_THREAD_EXITED() PERF_counter_add(kActiveThreads, -1)
#define PROFILE_APC_QUEUE_DEPTH(value) PERF_counter_set(kApcQueueDepth, value)
#define PROFILE_CRITICAL_REGION_CONTENTION() PERF_counter_inc(kCriticalRegionContentions)
#define PROFILE_TEXTURE_CACHE_HIT() PERF_counter_inc(kTextureCacheHits)
#define PROFILE_TEXTURE_CACHE_MISS() PERF_counter_inc(kTextureCacheMisses)
#define PROFILE_PIPELINE_CACHE_HIT() PERF_counter_inc(kPipelineCacheHits)
#define PROFILE_PIPELINE_CACHE_MISS() PERF_counter_inc(kPipelineCacheMisses)

// M1 baseline instrumentation
#define PROFILE_CP_BUSY_US(v) PERF_counter_add(kCpBusyUs, v)
#define PROFILE_CP_IDLE_US(v) PERF_counter_add(kCpIdleUs, v)
#define PROFILE_FENCE_WAIT_US(v) PERF_counter_add(kFenceWaitUs, v)
#define PROFILE_SWAP_US(v) PERF_counter_add(kSwapUs, v)
#define PROFILE_GPU_BUSY_US(v) PERF_counter_add(kGpuBusyUs, v)
#define PROFILE_SUBMISSION() PERF_counter_inc(kSubmissions)
#define PROFILE_RESOLVE() PERF_counter_inc(kResolves)
#define PROFILE_RT_SWITCH() PERF_counter_inc(kRenderTargetSwitches)
#define PROFILE_BARRIERS(n)             \
  do {                                  \
    PERF_counter_inc(kBarrierBatches);  \
    PERF_counter_add(kBarriers, n);     \
  } while (0)
#define PROFILE_PIPELINE_CREATED() PERF_counter_inc(kPipelinesCreated)
#define PROFILE_SHADER_TRANSLATED() PERF_counter_inc(kShaderTranslations)
#define PROFILE_DRAW_SKIPPED_PENDING() PERF_counter_inc(kDrawsSkippedPending)

// Scoped microsecond accumulator. Adds the elapsed wall time of the enclosing
// scope to `id`. Cheap enough (two QPC reads) to leave enabled in
// RelWithDebInfo builds on the GPU worker thread's coarse-grained scopes.
#define PERF_SCOPED_US(id) \
  rex::perf::ScopedMicrosecondAccumulator perf_scope_us_##id { rex::perf::CounterId::id }

#else

#define PERF_counter_set(id, value)
#define PERF_counter_inc(id)
#define PERF_counter_add(id, delta)

#define PROFILE_FRAME_TIME_US(value)
#define PROFILE_FPS(value)
#define PROFILE_FUNCTION_DISPATCHED()
#define PROFILE_INTERRUPT_DISPATCHED()
#define PROFILE_XMA_FRAME_DECODED()
#define PROFILE_DRAW_CALL()
#define PROFILE_VERTICES(n)
#define PROFILE_CMD_BUFFER_STALL()
#define PROFILE_AUDIO_LATENCY_US(value)
#define PROFILE_BUFFER_QUEUE_DEPTH(value)
#define PROFILE_THREAD_CREATED()
#define PROFILE_THREAD_EXITED()
#define PROFILE_APC_QUEUE_DEPTH(value)
#define PROFILE_CRITICAL_REGION_CONTENTION()
#define PROFILE_TEXTURE_CACHE_HIT()
#define PROFILE_TEXTURE_CACHE_MISS()
#define PROFILE_PIPELINE_CACHE_HIT()
#define PROFILE_PIPELINE_CACHE_MISS()

#define PROFILE_CP_BUSY_US(v)
#define PROFILE_CP_IDLE_US(v)
#define PROFILE_FENCE_WAIT_US(v)
#define PROFILE_SWAP_US(v)
#define PROFILE_GPU_BUSY_US(v)
#define PROFILE_SUBMISSION()
#define PROFILE_RESOLVE()
#define PROFILE_RT_SWITCH()
#define PROFILE_BARRIERS(n)
#define PROFILE_PIPELINE_CREATED()
#define PROFILE_SHADER_TRANSLATED()
#define PROFILE_DRAW_SKIPPED_PENDING()
#define PERF_SCOPED_US(id)

#endif
