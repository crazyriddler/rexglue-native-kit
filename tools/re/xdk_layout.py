"""Name XDK library functions in a new game by function-size layout.

XDK libraries are linked object file by object file, so the functions of one object
(e.g. d3d9i/draw.cpp: BeginVertices, DrawVerticesUP, DrawVertices, DrawIndexedVertices...)
appear consecutively and, for the same XDK build and link options, with identical sizes.
This tool slides every object's size sequence from a reference table over the game's
function list and reports runs of >= --min-run consecutive size matches.

It complements xdk_sigs.py (byte signatures from the Conan XDK, ~2006-2007): use it for
late-XDK games with tools/re/xdk_2012_dc3_symbols.tsv (Dance Central 3, Sep 2012).
Sizes only match when the XDK build matches; a game built with another XDK gives few or
no runs, which is itself the answer. A function is named only inside a run, so a lone
function whose linked neighbours differ from the reference (objects the game did not
link) stays unnamed: find it with xdk_sigs.py / pm4scan.py / callers instead. Tested on a
synthetic layout only (scripts/tests/test_xdk_layout.py); always confirm a hook target in
the disassembly.

usage:
  xdk_layout.py <register.cpp> [--image image.bin] [--ref xdk_2012_dc3_symbols.tsv]
                [--lib d3d9i,LIBCMT,xapilibi] [--min-run 3] [-o report.tsv]

<register.cpp>: generated/default/<game>_register.cpp (function starts from codegen).
--image: decoded image at 0x82000000; trailing zero padding is then removed from each
  function so sizes are exact. Without it a size may exceed the reference by < 16 bytes.
"""
import argparse
import os
import re
import struct
import sys

BASE = 0x82000000
HERE = os.path.dirname(os.path.abspath(__file__))


def read_starts(register_path):
    starts = set()
    for line in open(register_path, errors='replace'):
        m = re.search(r'SetFunction\(0x([0-9A-Fa-f]{8})', line)
        if m:
            starts.add(int(m.group(1), 16))
    return sorted(starts)


def game_functions(starts, image=None):
    """[(addr, size)] with size = distance to the next start, minus zero padding."""
    out = []
    for a, b in zip(starts, starts[1:]):
        size = b - a
        if image is not None:
            end = b
            while end - 4 > a:
                off = end - 4 - BASE
                if off < 0 or off + 4 > len(image) or struct.unpack_from('>I', image, off)[0] != 0:
                    break
                end -= 4
            size = end - a
        out.append((a, size))
    return out


def read_reference(path, libs=None):
    """{lib: [(name, size, object)]} in reference address order.

    One sequence per library, not per object: the objects of a library are linked
    consecutively, so runs may cross object boundaries (single-function objects such as
    LIBCMT memcpyp.cpp are only found that way). Objects the game did not link simply
    break a run, and matching restarts after them."""
    libs_out = {}
    for line in open(path, encoding='utf-8'):
        if line.startswith('#') or line.startswith('lib\t'):
            continue
        lib, obj, name, size, addr = line.rstrip('\n').split('\t')
        if libs and lib not in libs:
            continue
        libs_out.setdefault(lib, []).append((int(addr, 16), name, int(size, 16), obj))
    return {k: [(n, s, o) for _, n, s, o in sorted(v)] for k, v in libs_out.items()}


def size_ok(game_size, ref_size, exact):
    return game_size == ref_size if exact else 0 <= game_size - ref_size < 16


def match(funcs, reference, min_run=3, exact=False):
    """Return {addr: (name, lib, obj, run_len)} keeping the longest run per address.

    Run starts are tried only where the previous reference/game pair does not already
    match, so each maximal run is extended once (linear in practice)."""
    by_size = {}
    for i, (_, s) in enumerate(funcs):
        by_size.setdefault(s, []).append(i)
    best = {}
    for lib, seq in reference.items():
        if len(seq) < min_run:
            continue
        for j in range(len(seq) - min_run + 1):
            first = seq[j][1]
            cands = by_size.get(first, []) if exact else [
                i for d in range(0, 16, 4) for i in by_size.get(first + d, [])]
            for i in cands:
                if j > 0 and i > 0 and size_ok(funcs[i - 1][1], seq[j - 1][1], exact):
                    continue  # inside a run already extended from an earlier start
                k = 0
                while (j + k < len(seq) and i + k < len(funcs)
                       and size_ok(funcs[i + k][1], seq[j + k][1], exact)):
                    k += 1
                if k < min_run:
                    continue
                for t in range(k):
                    addr = funcs[i + t][0]
                    if addr not in best or best[addr][3] < k:
                        best[addr] = (seq[j + t][0], lib, seq[j + t][2], k)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('register')
    ap.add_argument('--image')
    ap.add_argument('--ref', default=os.path.join(HERE, 'xdk_2012_dc3_symbols.tsv'))
    ap.add_argument('--lib', default='d3d9i,LIBCMT,xapilibi',
                    help='comma list of reference libraries (d3d9i,LIBCMT,xapilibi,xgraphics,d3dx9)')
    ap.add_argument('--min-run', type=int, default=3)
    ap.add_argument('-o', '--out')
    a = ap.parse_args()
    image = open(a.image, 'rb').read() if a.image else None
    funcs = game_functions(read_starts(a.register), image)
    ref = read_reference(a.ref, set(a.lib.split(',')) if a.lib else None)
    best = match(funcs, ref, a.min_run, exact=image is not None)
    out = open(a.out, 'w') if a.out else sys.stdout
    out.write('addr\tname\tlib\tobject\trun\n')
    for addr in sorted(best):
        n, lib, obj, k = best[addr]
        out.write(f'0x{addr:08X}\t{n}\t{lib}\t{obj}\t{k}\n')
    print(f'{len(best)} functions named from {len(ref)} reference libraries '
          f'({len(funcs)} game functions)', file=sys.stderr)


if __name__ == '__main__':
    main()
