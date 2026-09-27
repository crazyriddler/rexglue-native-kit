#!/usr/bin/env python3
"""
Materialize Git symlinks that Git for Windows checked out as plain-text
placeholder files instead of real symlinks/junctions.

Root cause: this machine's user account has neither Administrator rights nor
Developer Mode enabled, so it lacks SeCreateSymbolicLinkPrivilege. Git for
Windows detects this at checkout time and, instead of failing, writes a small
text file containing the link target path in place of each symlinked file.
Any submodule that uses symlinks internally (e.g. libmspack's cabextract tool
directory symlinking shared mspack/*.c sources) ends up with unusable
placeholder files, which clang correctly reports as "expected identifier or
'('" because it's trying to compile a bare path string as C source.

This script walks a root directory, finds files under REXGLUE_SDK_GIT_ROOT
that `git ls-files -s` reports with mode 120000 (symlink) but that don't
actually contain the mode-120000 blob content on disk (i.e. got checked out
as plain text instead), and replaces each placeholder with a real copy of
its link target's content. This is a deterministic, reproducible fix — it
can be re-run any time after a fresh submodule checkout.
"""
import subprocess
import sys
from pathlib import Path


def find_symlink_entries(git_root: Path):
    """Yield (relative_path, link_target) for every git-tracked symlink."""
    out = subprocess.run(
        ["git", "ls-files", "-s"],
        cwd=git_root, capture_output=True, text=True, check=True,
    ).stdout
    for line in out.splitlines():
        mode, _rest = line.split(" ", 1)
        if mode != "120000":
            continue
        # format: "<mode> <sha> <stage>\t<path>"
        path = line.split("\t", 1)[1]
        yield Path(path)


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: fix_broken_symlinks.py <git checkout dir>")
    git_root = Path(sys.argv[1])
    fixed = []
    skipped_ok = []
    for rel_path in find_symlink_entries(git_root):
        full_path = git_root / rel_path
        if not full_path.exists():
            print(f"  ! missing entirely: {rel_path}")
            continue
        if full_path.is_symlink() or full_path.is_dir():
            skipped_ok.append(rel_path)
            continue
        content = full_path.read_bytes()
        # A real placeholder is a short text blob equal to the (relative)
        # link target path, no newlines.
        try:
            target_rel = content.decode("utf-8").strip()
        except UnicodeDecodeError:
            skipped_ok.append(rel_path)  # binary content -> not a placeholder
            continue
        if len(content) > 4096 or "\n" in target_rel or "\x00" in target_rel:
            skipped_ok.append(rel_path)
            continue
        target_path = (full_path.parent / target_rel).resolve()
        if not target_path.is_file():
            print(f"  ! placeholder target not found: {rel_path} -> {target_rel}")
            continue
        real_content = target_path.read_bytes()
        full_path.write_bytes(real_content)
        fixed.append(rel_path)

    print(f"Fixed {len(fixed)} placeholder file(s):")
    for p in fixed:
        print(f"  - {p}")
    print(f"({len(skipped_ok)} entries were already real symlinks/dirs/binaries, left alone)")


if __name__ == "__main__":
    main()
