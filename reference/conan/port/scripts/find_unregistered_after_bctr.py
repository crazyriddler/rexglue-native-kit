#!/usr/bin/env python3
"""
Find addresses immediately following a `bctr` instruction with NO zero-padding
gap before them, that are NOT registered as functions in
generated/default/conan_register.cpp.

Root cause class (seen twice now — 0x82280078/E006 and 0x82715FC0/E012):
gap-fill/function discovery reliably seeds a new function candidate right
after a `blr`-terminated function (there's usually a `.long 0x0` padding
instruction acting as a clear boundary marker), but does NOT reliably do the
same right after a `bctr` (indirect branch, unknown static target) with no
padding — the byte immediately following is real code (the next handler in
a chained dispatch-table processing sequence) but never gets registered
unless something else (a `bl`, a vtable slot) independently points at it.
These only crash at runtime, one at a time, whenever the game's actual data
happens to dispatch through that particular slot — so finding them all
statically up front beats discovering them one 20-second reboot at a time.
"""
import re
import subprocess
import sys
from pathlib import Path

DIS_PATH = Path(r"C:\Users\jrbar\conan-port\logs\default_full.dis")
REGISTER_CPP = Path(r"C:\Users\jrbar\conan-port\generated\default\conan_register.cpp")


def main():
    registered = set()
    for line in REGISTER_CPP.read_text(errors="replace").splitlines():
        m = re.search(r"SetFunction\(0x([0-9A-Fa-f]{8}),", line)
        if m:
            registered.add(m.group(1).upper())
    print(f"{len(registered)} functions registered in conan_register.cpp")

    candidates = []
    prev_addr = None
    prev_mnemonic = None
    with open(DIS_PATH, errors="replace") as f:
        for line in f:
            m = re.match(r"\s*(?:\d+:)?([0-9a-f]{8}):\s+[0-9a-f ]+\t(\S+)", line)
            if not m:
                continue
            addr, mnemonic = m.group(1).upper(), m.group(2)
            if prev_mnemonic == "bctr" and mnemonic != ".long":
                # addr immediately follows a bctr with no padding in between
                candidates.append(addr)
            prev_addr, prev_mnemonic = addr, mnemonic

    print(f"{len(candidates)} addresses immediately follow a bctr with no padding")

    missing = [a for a in candidates if a not in registered]
    print(f"{len(missing)} of those are NOT registered as functions:")
    for a in missing:
        print(f"  0x{a}")


if __name__ == "__main__":
    main()
