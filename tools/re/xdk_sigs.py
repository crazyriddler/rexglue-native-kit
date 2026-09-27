"""XDK Direct3D function finder: masked-instruction signatures.

Xbox 360 games statically link the XDK D3D library (device, ring buffer, draw
entry points, Resolve, Swap, tiling, shader creation...). The native renderer
hooks ~25 of those functions, and their addresses differ per game. This tool
builds signatures of the functions recovered in the reference port (Conan 2007,
XDK ~2006/2007, see reference/conan) and finds them in a new game's image.

Masking (context free, per 32-bit PPC word):
  b/bl (op 18)       keep opcode + AA/LK       (branch targets move)
  bc   (op 16)       keep opcode/BO/BI + AA/LK (displacements move)
  lis  (op 15)       imm16 masked when 0x7000..0x9FFF (absolute addresses)
  addi/ori and D-form loads/stores (op 14, 24, 32-55) with rA != r1:
                     imm16 masked (low halves of addresses; also struct
                     offsets, a small loss of specificity)
Everything else (opcodes, registers, stack offsets, other immediates) must match.

usage:
  xdk_sigs.py build  <image.bin> <register.cpp> <symbols.tsv> [...more .tsv] -o xdk_signatures.json
  xdk_sigs.py match  <image.bin> <register.cpp|-> [-s xdk_signatures.json] [-o report.tsv]

<image.bin>: the decoded executable image loaded at 0x82000000
  (scripts/port/xex_decode.py for basic compression, or run the built game with
   --dump_xex_image=<file> for any compression).
<register.cpp>: generated/default/<game>_register.cpp (function starts from
  codegen). '-' = heuristic function starts (after blr/padding, mflr prologues).
symbols .tsv: "0xADDR<TAB>name[<TAB>anything]" lines.

Match report: exact (all masked words equal) / fuzzy (best score >= 0.80 with a
margin over the runner-up) / ambiguous / missing. Always confirm a hook target
in the disassembly (callers, argument use, PM4 constants) before relying on it.
"""
import argparse
import json
import re
import struct
import sys

import numpy as np

BASE = 0x82000000
MAX_WORDS = 256


def load_image(path):
    data = open(path, 'rb').read()
    n = len(data) // 4
    return np.frombuffer(data[:n * 4], dtype='>u4').astype(np.uint32)


def mask_words(w):
    """Vectorized context-free mask of big-endian PPC words (uint32 array)."""
    w = w.astype(np.uint32)
    op = w >> 26
    ra = (w >> 16) & 31
    imm = w & 0xFFFF
    out = w.copy()
    out = np.where(op == 18, w & 0xFC000003, out)
    out = np.where(op == 16, w & 0xFFFF0003, out)
    lis_addr = (op == 15) & (imm >= 0x7000) & (imm <= 0x9FFF)
    out = np.where(lis_addr, w & 0xFFFF0000, out)
    dform = ((op == 14) | (op == 24) | ((op >= 32) & (op <= 55))) & (ra != 1)
    out = np.where(dform, w & 0xFFFF0000, out)
    return out.astype(np.uint32)


def function_starts(register_path, img):
    if register_path != '-':
        starts = set()
        for line in open(register_path, errors='replace'):
            m = re.search(r'SetFunction\(0x([0-9A-Fa-f]{8})', line)
            if m:
                starts.add(int(m.group(1), 16))
        return sorted(starts)
    # Heuristic: an instruction after blr / bctr / padding that is not padding,
    # plus every mflr r12 / mflr r0 prologue start.
    starts = set()
    prev = img[:-1]
    cur = img[1:]
    after = ((prev == 0x4E800020) | (prev == 0x4E800420) | (prev == 0)) & (cur != 0)
    for i in np.nonzero(after)[0]:
        starts.add(BASE + 4 * (int(i) + 1))
    for i in np.nonzero((img == 0x7D8802A6) | (img == 0x7C0802A6))[0]:
        starts.add(BASE + 4 * int(i))
    return sorted(starts)


def read_symbols(paths):
    syms = {}
    for p in paths:
        for line in open(p, encoding='utf-8', errors='replace'):
            parts = line.rstrip('\n').split('\t')
            if len(parts) >= 2 and parts[0].startswith('0x'):
                syms[int(parts[0], 16)] = parts[1]
    return syms


def cmd_build(a):
    img = load_image(a.image)
    masked = mask_words(img)
    starts = function_starts(a.register, img)
    syms = read_symbols(a.symbols)
    idx = {s: i for i, s in enumerate(starts)}
    out = []
    for addr, name in sorted(syms.items()):
        if addr not in idx:
            print(f'skip {addr:08X} {name}: not a function start', file=sys.stderr)
            continue
        i = idx[addr]
        end = starts[i + 1] if i + 1 < len(starts) else addr + 4 * MAX_WORDS
        n = min((end - addr) // 4, MAX_WORDS)
        o = (addr - BASE) // 4
        words = masked[o:o + n]
        out.append({'name': name, 'ref_addr': f'{addr:08X}', 'words': int(n),
                    'sig': ''.join(f'{int(x):08X}' for x in words)})
    json.dump({'format': 1, 'reference': a.reference, 'mask': 'context-free v1', 'functions': out},
              open(a.output, 'w'), indent=0)
    print(f'{len(out)} signatures -> {a.output}')


def cmd_match(a):
    img = load_image(a.image)
    masked = mask_words(img)
    starts = function_starts(a.register, img)
    db = json.load(open(a.signatures))
    starts_arr = np.array([(s - BASE) // 4 for s in starts if BASE <= s < BASE + 4 * len(img)], dtype=np.int64)
    K = 48  # words compared for the fuzzy scan
    pad = np.concatenate([masked, np.zeros(MAX_WORDS, dtype=np.uint32)])
    window = np.stack([pad[starts_arr + k] for k in range(K)], axis=1)  # [n_starts, K]
    lengths = np.diff(np.append(starts_arr, len(img)))
    rows = []
    for f in db['functions']:
        sig = np.array([int(f['sig'][i:i + 8], 16) for i in range(0, len(f['sig']), 8)], dtype=np.uint32)
        n = len(sig)
        k = min(K, n)
        score_k = (window[:, :k] == sig[:k]).mean(axis=1)
        order = np.argsort(-score_k)[:8]
        best = []
        for j in order:
            o = int(starts_arr[j])
            full = (pad[o:o + n] == sig).mean()
            # Penalize very different function lengths.
            ln = int(lengths[j])
            size_ok = 0.5 <= ln / max(n, 1) <= 2.0 or n == MAX_WORDS
            best.append((float(full) * (1.0 if size_ok else 0.9), BASE + 4 * o))
        best.sort(reverse=True)
        (s1, a1) = best[0]
        s2 = best[1][0] if len(best) > 1 else 0.0
        exact = [b for b in best if b[0] >= 0.9999]
        if len(exact) == 1:
            status = 'exact'
        elif len(exact) > 1:
            status = 'ambiguous'
        elif s1 >= 0.80 and s1 - s2 >= 0.05:
            status = 'fuzzy'
        elif s1 >= 0.80:
            status = 'ambiguous'
        else:
            status = 'missing'
        rows.append((f['name'], f['ref_addr'], f'{a1:08X}', f'{s1:.3f}', f'{s2:.3f}', status,
                     ' '.join(f'{b[1]:08X}' for b in exact) if status == 'ambiguous' else ''))
    lines = ['name\tref_addr\tbest_addr\tscore\trunner_up\tstatus\tcandidates']
    lines += ['\t'.join(r) for r in rows]
    text = '\n'.join(lines) + '\n'
    if a.output:
        open(a.output, 'w').write(text)
    counts = {}
    for r in rows:
        counts[r[5]] = counts.get(r[5], 0) + 1
    print(text if not a.output else f'report -> {a.output}')
    print('summary:', ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('image'); b.add_argument('register'); b.add_argument('symbols', nargs='+')
    b.add_argument('-o', '--output', default='xdk_signatures.json')
    b.add_argument('--reference', default='Conan (2007, Nihilistic) default.xex, XDK ~2006/2007')
    m = sub.add_parser('match')
    m.add_argument('image'); m.add_argument('register')
    m.add_argument('-s', '--signatures', default=__file__.replace('xdk_sigs.py', 'xdk_signatures.json'))
    m.add_argument('-o', '--output')
    a = ap.parse_args()
    cmd_build(a) if a.cmd == 'build' else cmd_match(a)


if __name__ == '__main__':
    main()
