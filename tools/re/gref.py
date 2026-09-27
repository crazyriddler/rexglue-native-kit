"""Resolve lis-based absolute addresses (linear, per-function). gref.py ADDR [ADDR...] -> refs"""
import re, sys, collections
from disdb import DB
db = DB.load()
MEM = re.compile(r'^(r\d+|f\d+),(-?\d+)\((r\d+)\)$')
ADDI = re.compile(r'^(r\d+),(r\d+),(-?\d+)$')
def refs(f):
    hi = {}
    for a, (mn, ops) in db.body(f):
        if mn == 'lis':
            m = re.match(r'(r\d+),(-?\d+)', ops)
            if m: hi[m.group(1)] = ((int(m.group(2)) & 0xffff) << 16); continue
        m = MEM.match(ops)
        if m and m.group(3) in hi and mn[0] in 'ls':
            yield a, mn, (hi[m.group(3)] + int(m.group(2))) & 0xffffffff
            if mn.startswith('l') and m.group(1) == m.group(3): hi.pop(m.group(3), None)
            elif mn.startswith('l'): hi.pop(m.group(1), None)
            continue
        m = ADDI.match(ops)
        if m and mn == 'addi' and m.group(2) in hi:
            v = (hi[m.group(2)] + int(m.group(3))) & 0xffffffff
            yield a, mn, v
            hi.pop(m.group(1), None); continue
        # any other write to a reg kills it
        m = re.match(r'(r\d+),', ops)
        if m and mn not in ('stw','stb','sth','std','cmpw','cmplw','cmpwi','cmplwi') and not mn.startswith('st'):
            hi.pop(m.group(1), None)
def allrefs():
    d = collections.defaultdict(list)
    for f in db.funcs:
        if f in db.names: continue
        for a, mn, v in refs(f): d[v].append((a, mn, f))
    return d
if __name__ == '__main__':
    import pickle, os
    from disdb import PORT
    cache = os.path.join(PORT, 'logs', 'gref.pkl')
    if os.path.exists(cache): d = pickle.load(open(cache, 'rb'))
    else: d = allrefs(); pickle.dump(dict(d), open(cache, 'wb'))
    for x in sys.argv[1:]:
        t = int(x, 16); l = d.get(t, [])
        fs = collections.Counter(f for a, mn, f in l)
        print('%08X: %d refs in %d funcs; kinds %s' % (t, len(l), len(fs), dict(collections.Counter(mn for a, mn, f in l))))
        for f, n in fs.most_common(400): print('   %s x%d' % (db.name(f), n))
