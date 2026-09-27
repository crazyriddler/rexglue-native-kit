// Hang watchdog: when guest swaps stop for native_hang_watchdog_s seconds, logs
// the native call stacks of every process thread once (dbghelp, conan.pdb).
// Diagnoses freezes without an attached debugger.
#include "hang_watchdog.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(native_hang_watchdog_s, 6, "Conan",
                     "Log all thread stacks when no swap happens for this many seconds (0 = off)");

namespace conan::native {
namespace {

std::atomic<int64_t> g_last_beat_ms{0};
std::atomic<uint32_t> g_beat_thread{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void DumpThread(HANDLE process, DWORD tid, bool is_render) {
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                             FALSE, tid);
  if (!thread) return;
  if (SuspendThread(thread) == DWORD(-1)) {
    CloseHandle(thread);
    return;
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_FULL;
  std::string out = fmt::format("native watchdog: thread {}{}:", tid, is_render ? " (swap thread)" : "");
  if (GetThreadContext(thread, &ctx)) {
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24; ++i) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
                       SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
          !frame.AddrPC.Offset) {
        break;
      }
      alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
      auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
      sym->SizeOfStruct = sizeof(SYMBOL_INFO);
      sym->MaxNameLen = 255;
      DWORD64 disp = 0;
      if (SymFromAddr(process, frame.AddrPC.Offset, &disp, sym)) {
        out += fmt::format("\n    {}+0x{:X}", sym->Name, disp);
      } else {
        out += fmt::format("\n    0x{:X}", frame.AddrPC.Offset);
      }
    }
  }
  ResumeThread(thread);
  CloseHandle(thread);
  REXLOG_WARN("{}", out);
}

void DumpAllThreads() {
  HANDLE process = GetCurrentProcess();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(process, nullptr, TRUE);
  DWORD self = GetCurrentThreadId();
  DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
    DumpThread(process, te.th32ThreadID, te.th32ThreadID == g_beat_thread.load());
  }
  CloseHandle(snap);
  SymCleanup(process);
  rex::FlushLogging();
}

void WatchdogMain() {
  bool reported = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    int32_t limit_s = REXCVAR_GET(native_hang_watchdog_s);
    int64_t last = g_last_beat_ms.load();
    if (limit_s <= 0 || !last) continue;
    int64_t idle = NowMs() - last;
    if (idle < int64_t(limit_s) * 1000) {
      reported = false;
      continue;
    }
    if (!reported) {
      reported = true;
      REXLOG_WARN("native watchdog: no swap for {} ms, dumping thread stacks", idle);
      DumpAllThreads();
    }
  }
}

}  // namespace

void HangWatchdogBeat() {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(WatchdogMain).detach(); });
  g_beat_thread.store(GetCurrentThreadId());
  g_last_beat_ms.store(NowMs());
}

}  // namespace conan::native
