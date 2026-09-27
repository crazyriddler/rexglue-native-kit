"""Semantics test for the kit SDK codegen's sraw/srad emitters (XER.CA).

Renders the format strings of build_sraw/build_srad in sdk/src/codegen/builders/logical.cpp
into C++, compiles them next to a PowerPC ISA 2.02 reference and compares result + CA for
every shift count on edge values. Regression for the "0x80000000 with shift >= 32 cleared
CA" defect (found by LostOdysseyRecomp's recompiler audit in XenonRecomp, present in the
ReXGlue fork until 2026-09-27; sdk/KIT_SDK_CHANGES.md). Skips when no C++ compiler exists.
"""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
BUILDERS = REPO_ROOT / "sdk" / "src" / "codegen" / "builders" / "logical.cpp"
NAMES = {"ctx.xer()": "xer", "ctx.temp()": "temp", "ctx.r(ctx.insn.operands[0])": "rd",
         "ctx.r(ctx.insn.operands[1])": "rs", "ctx.r(ctx.insn.operands[2])": "rb"}


def render(src, fn):
    body = re.search(r"bool " + fn + r"\(BuilderContext& ctx\) \{(.*?)\n\}", src, re.S).group(1)
    lines = []
    for m in re.finditer(r'ctx\.println\(\s*"(.*?)",\s*(.*?)\);', body, re.S):
        text = m.group(1).replace("\\t", "\t").replace("{{", "{").replace("}}", "}")
        args = [NAMES[a.strip()] for a in m.group(2).replace("\n", " ").split(",")]
        lines.append(text.replace("{}", "%s") % tuple(args))
    return "\n".join(lines)


HARNESS = r"""#include <cstdint>
#include <cstdio>
union R { int64_t s64; uint64_t u64; int32_t s32; uint32_t u32; };
struct X { uint8_t ca; };
static void sraw(R rs, R rb, R& rd, X& xer) { R temp; @SRAW@ }
static void srad(R rs, R rb, R& rd, X& xer) { R temp; @SRAD@ }
int main() {
  int bad = 0;
  const uint32_t v32[] = {0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0u, 1u, 0xC0000000u};
  for (uint32_t v : v32) for (uint32_t n = 0; n < 64; ++n) {
    R rs; rs.u64 = 0xDEAD000000000000ull | v; R rb; rb.u64 = n; R rd; X x; sraw(rs, rb, rd, x);
    int64_t s = int32_t(v), er; int ec;
    if (n >= 32) { er = s < 0 ? -1 : 0; ec = s < 0; }
    else { er = s >> n; ec = s < 0 && n && (v & ((1ull << n) - 1)); }
    if (rd.s64 != er || x.ca != ec) { printf("sraw %08X n=%u\n", v, n); ++bad; }
  }
  const uint64_t v64[] = {0x8000000000000000ull, 0x8000000000000001ull, ~0ull, 0x7FFFFFFFFFFFFFFFull, 0ull, 1ull};
  for (uint64_t v : v64) for (uint32_t n = 0; n < 128; ++n) {
    R rs; rs.u64 = v; R rb; rb.u64 = n; R rd; X x; srad(rs, rb, rd, x);
    int64_t s = int64_t(v), er; int ec;
    if (n >= 64) { er = s < 0 ? -1 : 0; ec = s < 0; }
    else { er = s >> n; ec = s < 0 && n && (v & ((1ull << n) - 1)); }
    if (rd.s64 != er || x.ca != ec) { printf("srad %016llX n=%u\n", (unsigned long long)v, n); ++bad; }
  }
  printf("%d mismatches\n", bad);
  return bad != 0;
}
"""


def test_sraw_srad_carry(tmp_path):
    cxx = shutil.which("clang++") or shutil.which("g++")
    if not cxx:
        pytest.skip("no C++ compiler")
    src = BUILDERS.read_text(encoding="utf-8")
    code = HARNESS.replace("@SRAW@", render(src, "build_sraw")).replace("@SRAD@", render(src, "build_srad"))
    cpp, exe = tmp_path / "sra.cpp", tmp_path / "sra.exe"
    cpp.write_text(code)
    subprocess.run([cxx, "-std=c++20", "-O2", str(cpp), "-o", str(exe)], check=True)
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout
