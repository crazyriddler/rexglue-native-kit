"""Caller/callee breakdown from the full stacks in a --sample_profile_out file.
usage: profile_stacks.py profile.txt <slot> <function-substring> [top]
For samples whose stack contains a frame matching the substring (outermost
match), prints: % of the slot's samples, the immediate callees below it, the
leaf functions under it, and its immediate callers."""
import sys, subprocess, collections, os, json, bisect

from kitcfg import LLVM_SYMBOLIZER as LLVM
path, slot, pat = sys.argv[1], int(sys.argv[2]), sys.argv[3]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 25

modules = []  # (base, size, path)
stacks = []
total = 0
for line in open(path):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'module':
        modules.append((int(p[1], 16), int(p[2], 16), p[3]))
    elif p[0] == 'stack' and int(p[1]) == slot:
        stacks.append((int(p[2]), [int(x, 16) for x in p[3].split(';')]))
    elif p[0] == 'slot' and int(p[1]) == slot:
        total = int(p[2])
modules.sort()
bases = [m[0] for m in modules]


def module_of(a):
    i = bisect.bisect_right(bases, a) - 1
    if i >= 0 and a < modules[i][0] + modules[i][1]:
        return modules[i]
    return None


# Symbolize every distinct address (return addresses: -1 to land in the call).
addrs = set()
for _, fr in stacks:
    for i, a in enumerate(fr):
        addrs.add((a, i > 0))
bymod = collections.defaultdict(set)
for a, ret in addrs:
    m = module_of(a)
    if m:
        bymod[m].add(a - m[0] - (1 if ret else 0))
names = {}
for m, rvas in bymod.items():
    base = os.path.basename(m[2]).split('.')[0]
    rl = sorted(rvas)
    res = {}
    if os.path.exists(m[2]):
        out = subprocess.run([LLVM, '--obj=' + m[2], '--functions=short', '--no-inlines',
                              '--relative-address', '--output-style=JSON'],
                             input='\n'.join(hex(r) for r in rl), capture_output=True,
                             text=True).stdout
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
    return names.get((m[0], a - m[0] - (1 if ret else 0)), hex(a))


callees = collections.Counter()
leaves = collections.Counter()
callers = collections.Counter()
hit = 0
for count, fr in stacks:
    syms = [name(a, i > 0) for i, a in enumerate(fr)]
    # outermost matching frame
    idx = max((i for i, s in enumerate(syms) if pat in s), default=None)
    if idx is None:
        continue
    hit += count
    callees[syms[idx - 1] if idx > 0 else '<self>'] += count
    leaves[syms[0]] += count
    callers[syms[idx + 1] if idx + 1 < len(syms) else '<root>'] += count

print(f"slot {slot}: '{pat}' in {hit} of {total} samples ({100*hit/max(total,1):.2f}%)")
for title, c in (('callees', callees), ('leaves', leaves), ('callers', callers)):
    print(f"--- {title}")
    for k, v in c.most_common(top):
        print(f"  {100*v/max(total,1):6.2f}%  {k}")
