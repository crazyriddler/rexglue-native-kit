// Release builds target x86-64-v3 (AVX2/FMA/BMI2, EXP-047). This translation
// unit is compiled for baseline x86-64 (CMakeLists.txt) and runs before any
// other C++ initializer, so an older CPU gets a message instead of an
// illegal-instruction crash.
#ifdef _WIN32
#include <intrin.h>
#include <windows.h>

namespace {

bool CpuSupportsX86_64V3() {
  int r[4];
  __cpuid(r, 0);
  if (r[0] < 7) return false;
  __cpuid(r, 1);
  const int ecx1 = r[2];
  const bool fma = ecx1 & (1 << 12), movbe = ecx1 & (1 << 22), osxsave = ecx1 & (1 << 27),
             avx = ecx1 & (1 << 28), f16c = ecx1 & (1 << 29);
  if (!(fma && movbe && osxsave && avx && f16c)) return false;
  // The OS must save the YMM state.
  if ((_xgetbv(0) & 6) != 6) return false;
  __cpuidex(r, 7, 0);
  const int ebx7 = r[1];
  const bool bmi1 = ebx7 & (1 << 3), avx2 = ebx7 & (1 << 5), bmi2 = ebx7 & (1 << 8);
  __cpuid(r, 0x80000001);
  const bool lzcnt = r[2] & (1 << 5);
  return bmi1 && avx2 && bmi2 && lzcnt;
}

struct CpuCheck {
  CpuCheck() {
    if (CpuSupportsX86_64V3()) return;
    const bool spanish = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    MessageBoxW(nullptr,
                spanish ? L"Este procesador no es compatible: se necesita una CPU con AVX2 "
                          L"(Intel Haswell / AMD Zen o posterior)."
                        : L"This processor is not supported: a CPU with AVX2 "
                          L"(Intel Haswell / AMD Zen or newer) is required.",
                L"Conan", MB_OK | MB_ICONERROR);
    ExitProcess(1);
  }
};

__attribute__((init_priority(101))) CpuCheck g_cpu_check;

}  // namespace
#endif
