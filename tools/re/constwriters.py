"""Find functions that compute device VS/PS float-constant addresses: (reg+120)*16 (VS, dev+0x780)
or (reg+376)*16 (PS, dev+0x1780)."""
import re, collections
from disdb import DB
db = DB.load(); c = db.calls()
hits = collections.defaultdict(list)
for f in db.funcs:
    prev = {}
    for a, (mn, ops) in db.body(f):
        m = re.match(r'^(r\d+),(r\d+),(120|376)$', ops)
        if mn == 'addi' and m: prev[m.group(1)] = (a, m.group(3)); continue
        m = re.match(r'^(r\d+),(r\d+),4,0,27$', ops)
        if mn == 'rlwinm' and m and m.group(2) in prev:
            hits[f].append(('VS' if prev[m.group(2)][1] == '120' else 'PS', '%08X' % a))
for f, l in sorted(hits.items()):
    print('%08X callers=%d %s' % (f, len(c.get(f, [])), l[:6]))
