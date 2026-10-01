"""find_adjustor_thunks.py on real objdump output of a small PPC image (kit binutils)."""
import os
import platform
import subprocess
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "port"))
import find_adjustor_thunks as fat  # noqa: E402

EXE = ".exe" if platform.system() == "Windows" else ""
AS = os.path.join(ROOT, "tools", "binutils", "powerpc-none-elf-as" + EXE)
OBJDUMP = os.path.join(ROOT, "tools", "binutils", "powerpc-none-elf-objdump" + EXE)
OBJCOPY = os.path.join(ROOT, "tools", "binutils", "powerpc-none-elf-objcopy" + EXE)
BASE = 0x82000000

# 0x00 method A, 0x08 method B, 0x10 padding, 0x14-0x2b packed thunk block (3 thunks),
# 0x2c padding, 0x30 lone thunk, 0x38 function with an `addi r3; b` tail call (not a thunk).
SRC = """
  .text
  .globl _start
_start:
methodA: li r3, 1
  blr
methodB: li r3, 2
  blr
  .long 0
  addi r3, r3, -8
  b methodA
  addi r3, r3, -16
  b methodB
  addi r3, r3, -24
  b methodA
  .long 0
  addi r3, r3, -4
  b methodB
func: mflr r0
  addi r3, r3, -8
  b methodA
"""


@pytest.fixture(scope="module")
def dis(tmp_path_factory):
    if not (os.access(AS, os.X_OK) and os.access(OBJDUMP, os.X_OK)):
        pytest.skip("kit binutils not runnable on this host")
    d = tmp_path_factory.mktemp("thunks")
    (d / "t.s").write_text(SRC)
    subprocess.run([AS, "-mregnames", "-o", str(d / "t.o"), str(d / "t.s")], check=True)
    subprocess.run([OBJCOPY, "-O", "binary", "-j", ".text", str(d / "t.o"), str(d / "t.bin")],
                   check=True)
    # Same command as GAME_ADAPTATION_GUIDE §1 uses for $PORT_DIR/logs/default_full.dis.
    out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "powerpc", "-EB",
                          f"--adjust-vma={BASE:#x}", str(d / "t.bin")], check=True,
                         capture_output=True, text=True).stdout
    (d / "t.dis").write_text(out)
    return d


def test_finds_packed_and_lone_thunks_only(dis):
    insns = fat.parse_dis(dis / "t.dis")
    found = {a: (adj, tgt, conf) for a, adj, tgt, conf in fat.classify(insns, fat.find_thunks(insns))}
    assert found == {
        BASE + 0x14: (-8, BASE + 0x00, "high"),
        BASE + 0x1C: (-16, BASE + 0x08, "high"),
        BASE + 0x24: (-24, BASE + 0x00, "high"),
        BASE + 0x30: (-4, BASE + 0x08, "check"),
    }


def test_registered_thunks_are_not_reported(dis, capsys):
    (dis / "reg.cpp").write_text(
        "SetFunction(0x82000000, x);\nSetFunction(0x82000008, x);\nSetFunction(0x8200001C, x);\n")
    fat.main([str(dis / "t.dis"), str(dis / "reg.cpp"), "--toml"])
    out = capsys.readouterr().out
    assert "[entrypoint.functions.0x8200001C]" not in out
    assert "[entrypoint.functions.0x82000014]\nend = 0x8200001C" in out
    assert "# 4 adjustor thunks, 3 not registered (2 high confidence)" in out
