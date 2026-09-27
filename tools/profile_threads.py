"""Per-thread summary of a --sample_profile_out file (all slots; use with
--sample_profile_all_threads). For each thread: samples, share of "active"
samples (leaf not in the OS: ntdll/kernelbase/win32u wait or sleep) and the top
active leaf functions plus the top frames under which active samples land.
usage: profile_threads.py profile.txt [min_active_pct=2] [top=8]"""
import bisect
import collections
import json
import os
import subprocess
import sys

from kitcfg import LLVM_SYMBOLIZER as LLVM
path = sys.argv[1]
min_active = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
top = int(sys.argv[3]) if len(sys.argv) > 3 else 8

modules, slots, stacks = [], {}, collections.defaultdict(list)
for line in open(path):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'module':
        modules.append((int(p[1], 16), int(p[2], 16), p[3]))
    elif p[0] == 'slot':
        slots[int(p[1])] = (int(p[2]), p[3] if len(p) > 3 else '')
    elif p[0] == 'stack':
        stacks[int(p[1])].append((int(p[2]), [int(x, 16) for x in p[3].split(';')]))
modules.sort()
bases = [m[0] for m in modules]


def module_of(a):
    i = bisect.bisect_right(bases, a) - 1
    if i >= 0 and a < modules[i][0] + modules[i][1]:
        return modules[i]
    return None


OS_MODULES = ('ntdll', 'kernelbase', 'win32u', 'kernel32')
addrs = collections.defaultdict(set)
for sl in stacks.values():
    for _, fr in sl:
        for i, a in enumerate(fr[:12]):
            m = module_of(a)
            if m:
                addrs[m].add(a - m[0] - (1 if i else 0))
names = {}
for m, rvas in addrs.items():
    base = os.path.basename(m[2]).split('.')[0]
    rl = sorted(rvas)
    res = {}
    if os.path.exists(m[2]) and base.lower() not in OS_MODULES:
        out = subprocess.run([LLVM, '--obj=' + m[2], '--functions=short', '--no-inlines',
                              '--relative-address', '--output-style=JSON'],
                             input='\n'.join(hex(r) for r in rl), capture_output=True, text=True).stdout
        for l in out.splitlines():
            try:
                j = json.loads(l)
            except Exception:
                continue
            for e in (j if isinstance(j, list) else [j]):
                try:
                    a = int(e.get('Address', '0'), 16)
                except Exception:
                    continue
                fr = e.get('Symbol') or []
                n = fr[0].get('FunctionName', '') if fr else ''
                res[a] = n
    for r in rl:
        n = res.get(r) or ''
        names[(m[0], r)] = base + '!' + (n if n and n != '??' else hex(r))


def name(a, ret):
    m = module_of(a)
    if not m:
        return hex(a)
    return names.get((m[0], a - m[0] - (1 if ret else 0)), os.path.basename(m[2]) + '!' + hex(a - m[0]))


def is_os(a):
    m = module_of(a)
    return m is not None and os.path.basename(m[2]).split('.')[0].lower() in OS_MODULES


rows = []
for slot, (total, tname) in slots.items():
    active = collections.Counter()
    under = collections.Counter()
    n_active = 0
    for count, fr in stacks[slot]:
        if is_os(fr[0]):
            continue
        n_active += count
        active[name(fr[0], False)] += count
        seen = set()
        for i, a in enumerate(fr[1:10], 1):
            nm = name(a, True)
            if nm not in seen:
                seen.add(nm)
                under[nm] += count
    rows.append((n_active / max(1, total) * 100, slot, tname, total, active, under, n_active))
rows.sort(reverse=True)
for pct, slot, tname, total, active, under, n_active in rows:
    if pct < min_active:
        continue
    print(f'== slot {slot} {tname}: active {pct:.1f}% of {total} samples')
    for nm, c in active.most_common(top):
        print(f'   leaf {100 * c / total:5.1f}%  {nm}')
    for nm, c in under.most_common(top):
        print(f'   under {100 * c / total:5.1f}%  {nm}')
