#!/usr/bin/env python3
"""
List MSVC `this`-adjustor thunks that codegen did not register as functions.

Multiple inheritance makes MSVC emit 8-byte thunks referenced only from vtables:

    addi  r3,r3,-N      # move `this` to the base the method expects
    b     Method        # tail-jump to the real implementation

When the vtable holding the thunk is not discovered, the thunk is not registered and the
first virtual call through it dies with "Call to invalid or unregistered function at
guest address 0x...". Unlike the after-`bctr` candidates (find_unregistered_after_bctr.py,
fix reactively), this pattern is exact, so the whole list can be registered at once.

A pair is a thunk when the instruction before it ends control flow (b, blr, bctr, a padding
word) or another thunk: an `addi r3,r3,N; b` inside a function is a tail call, not a thunk.
"high" = bracketed by another thunk (packed thunk blocks have no padding); "check" = lone
thunk, confirm it in the disassembly before registering.

usage: python scripts/port/find_adjustor_thunks.py [--toml] [<disassembly> <register.cpp>]
  default inputs: $PORT_DIR/logs/default_full.dis (objdump -D of the decoded image) and
  $PORT_DIR/generated/default/<GAME_NAME>_register.cpp. --toml prints manifest entries.
Idea: VivaPinataRecomp tools/find_thunk_holes.py (reimplemented for the kit's inputs).
"""
import re
import sys
from pathlib import Path
import os as _os
_sys_path = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "..", "tools")
sys.path.insert(0, _sys_path)

LINE = re.compile(r"\s*(?:\d+:)?([0-9a-fA-F]{1,8}):\s+(?:[0-9a-fA-F]{2} ?){4}\s*\t(\S+)\s*([^<\n]*)")
ADDI_R3 = re.compile(r"r3,\s*r3,\s*(-?(?:0x[0-9a-fA-F]+|\d+))$")
B_TARGET = re.compile(r"^(?:0x)?([0-9a-fA-F]+)")  # "0x82..." (binary dump) or "82... <sym>"
ENDS_FLOW = {"b", "blr", "bctr", ".long"}


def parse_dis(path):
    """[(addr, mnemonic, operands)] in address order."""
    out = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if m:
                out.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return out


def registered_functions(path):
    regs = set()
    for line in Path(path).read_text(errors="replace").splitlines():
        m = re.search(r"SetFunction\(0x([0-9A-Fa-f]{8}),", line)
        if m:
            regs.add(int(m.group(1), 16))
    return regs


def find_thunks(insns):
    """Return {addr: (adjust, target)} for every addi r3,r3,N + b pair."""
    thunks = {}
    for i in range(len(insns) - 1):
        a, mn, ops = insns[i]
        a2, mn2, ops2 = insns[i + 1]
        if mn != "addi" or mn2 != "b" or a2 != a + 4:
            continue
        m = ADDI_R3.match(ops)
        t = B_TARGET.match(ops2)
        if not m or not t:
            continue
        adj = int(m.group(1), 0)
        if adj == 0:
            continue
        thunks[a] = (adj, int(t.group(1), 16))
    return thunks


def classify(insns, thunks):
    """Keep thunks whose predecessor ends control flow; rank by bracketing."""
    prev = {insns[i][0]: insns[i - 1] for i in range(1, len(insns))}
    result = []
    for a, (adj, target) in sorted(thunks.items()):
        p = prev.get(a)
        if p is None:
            continue
        p_addr, p_mn, p_ops = p
        if (p_addr - 4) not in thunks and p_mn not in ENDS_FLOW:
            continue  # tail call inside a function
        if p_mn == ".long" and p_ops not in ("0x0", "0"):
            continue  # data before it
        bracketed = (a - 8) in thunks or (a + 8) in thunks
        result.append((a, adj, target, "high" if bracketed else "check"))
    return result


def main(argv):
    toml = "--toml" in argv
    args = [x for x in argv if x != "--toml"]
    if len(args) == 2:
        dis, reg = args
    else:
        from kitcfg import PORT, GAME  # noqa: E402
        dis = Path(PORT) / "logs" / "default_full.dis"
        reg = Path(PORT) / "generated" / "default" / f"{GAME}_register.cpp"
    insns = parse_dis(dis)
    regs = registered_functions(reg)
    found = classify(insns, find_thunks(insns))
    missing = [t for t in found if t[0] not in regs]
    print(f"# {len(found)} adjustor thunks, {len(missing)} not registered "
          f"({sum(1 for t in missing if t[3] == 'high')} high confidence)")
    for a, adj, target, conf in missing:
        note = f"this{adj:+d} -> 0x{target:08X}{'' if target in regs else ' (target unregistered)'}"
        if toml:
            print(f"[entrypoint.functions.0x{a:08X}]\nend = 0x{a + 8:08X}  # {conf}: {note}")
        else:
            print(f"0x{a:08X}  {conf:5}  {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
