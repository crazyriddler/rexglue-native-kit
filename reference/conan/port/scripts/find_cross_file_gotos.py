#!/usr/bin/env python3
"""
Scan every generated conan_recomp.*.cpp for `goto loc_XXXXXXXX;` whose target
label isn't defined in the same file. A raw C++ goto can't cross translation
units, so every one of these is a build error waiting to happen (or already
is) — this catches the whole error class in one pass instead of discovering
them one slow rebuild at a time.

For each cross-file goto, also reports which DEFINE_REX_FUNC the goto lives in
and which one the label lives in, since that's exactly the (child, parent)
pair a manifest [entrypoint.functions.0xCHILD] parent = 0xPARENT chunk
override needs.
"""
import re
import sys
from pathlib import Path

GOTO_RE = re.compile(r"goto\s+loc_([0-9A-Fa-f]{8});")
LABEL_RE = re.compile(r"^loc_([0-9A-Fa-f]{8}):")
FUNC_RE = re.compile(r"DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)")


def scan_file(path: Path):
    labels = set()
    gotos = []  # (target, line_no, enclosing_func)
    current_func = None
    for line_no, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        m = FUNC_RE.search(line)
        if m:
            current_func = m.group(1).upper()
        m = LABEL_RE.match(line.strip())
        if m:
            labels.add(m.group(1).upper())
        for m in GOTO_RE.finditer(line):
            gotos.append((m.group(1).upper(), line_no, current_func))
    return labels, gotos


def main():
    gen_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(r"C:\Users\jrbar\conan-port\generated\default")

    file_labels = {}
    file_gotos = {}
    for f in sorted(gen_dir.glob("conan_recomp.*.cpp")):
        labels, gotos = scan_file(f)
        file_labels[f] = labels
        file_gotos[f] = gotos

    # Build global label -> (file, enclosing func at definition time) map
    # (re-scan to know which func each label belongs to, since a file can
    # define many functions)
    label_owner = {}  # LABEL -> (file, func)
    for f in sorted(gen_dir.glob("conan_recomp.*.cpp")):
        current_func = None
        for line in f.read_text(errors="replace").splitlines():
            m = FUNC_RE.search(line)
            if m:
                current_func = m.group(1).upper()
            m = LABEL_RE.match(line.strip())
            if m:
                label_owner[m.group(1).upper()] = (f.name, current_func)

    problems = []
    for f, gotos in file_gotos.items():
        local_labels = file_labels[f]
        for target, line_no, func in gotos:
            if target not in local_labels:
                owner = label_owner.get(target, ("UNKNOWN", None))
                problems.append((f.name, line_no, func, target, owner[0], owner[1]))

    print(f"Found {len(problems)} cross-file goto(s):")
    pairs = set()
    for fname, line_no, func, target, owner_file, owner_func in problems:
        print(f"  {fname}:{line_no}  sub_{func} -> goto loc_{target}  "
              f"(label lives in {owner_file}, inside sub_{owner_func})")
        if func and owner_func:
            pairs.add((func, owner_func))

    print()
    print(f"Distinct (child-with-goto, label-owner-func) pairs: {len(pairs)}")
    for child, parent in sorted(pairs):
        print(f"  sub_{child} falls through into sub_{parent}")


if __name__ == "__main__":
    main()
