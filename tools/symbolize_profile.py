"""Symbolize a sample profile written by --sample_profile_out.
usage: symbolize_profile.py profile.txt [topN]
Prints per thread slot: module split, top exclusive and top inclusive functions."""
import sys, subprocess, collections, os, json

from kitcfg import LLVM_SYMBOLIZER as LLVM
path = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 40

slots = {}
rows = []
for line in open(path):
    p = line.rstrip('\n').split('\t')
    if p[0] == 'slot':
        slots[int(p[1])] = (int(p[2]), p[3])
    else:
        rows.append((p[0], int(p[1]), int(p[2]), p[3], int(p[4], 16)))

bymod = collections.defaultdict(set)
for k, s, c, m, r in rows:
    bymod[m].add(r)

sym = {}
for m, rvas in bymod.items():
    base = os.path.basename(m).split('.')[0]
    rl = sorted(rvas)
    names = {}
    if os.path.exists(m):
        inp = '\n'.join(hex(r) for r in rl)
        out = subprocess.run([LLVM, '--obj=' + m, '--functions=short', '--no-inlines',
                              '--relative-address', '--output-style=JSON'],
                             input=inp, capture_output=True, text=True).stdout
        for line in out.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                j = json.loads(line)
            except Exception:
                continue
            for e in (j if isinstance(j, list) else [j]):
                try:
                    a = int(e.get('Address', '0'), 16)
                except Exception:
                    continue
                fr = e.get('Symbol') or []
                names[a] = fr[0].get('FunctionName', '') if fr else ''
    for r in rl:
        n = names.get(r) or ''
        if n in ('', '??'):
            n = hex(r)
        sym[(m, r)] = base + '!' + n

for slot, (n, name) in sorted(slots.items()):
    for kind in ('excl', 'incl'):
        agg = collections.Counter()
        mods = collections.Counter()
        for k, s, c, m, r in rows:
            if k == kind and s == slot:
                agg[sym[(m, r)]] += c
                mods[os.path.basename(m)] += c
        print(f"\n=== slot {slot} {name} ({n} samples) {kind} ===")
        if kind == 'excl':
            print('  modules: ' + ', '.join(f"{m} {100*c/n:.1f}%" for m, c in mods.most_common(8)))
        for f, c in agg.most_common(top):
            print(f"  {100*c/n:6.2f}%  {f}")
