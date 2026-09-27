"""Show constant argument registers set before each call site of a function.
callargs.py FUNC [regs=r4,r5,...]"""
import sys, re
from disdb import DB
db = DB.load(); c = db.calls()
t = int(sys.argv[1], 16)
regs = (sys.argv[2] if len(sys.argv) > 2 else 'r4,r5,r6,r7,r8').split(',')
for s in sorted(c.get(t, [])):
    vals = {}
    a = s - 4; f = db.func_of(s)
    while a >= max(f, s - 120) and len(vals) < len(regs):
        mn, ops = db.ins.get(a, ('', ''))
        m = re.match(r'^(r\d+),?(.*)$', ops)
        if mn in ('bl', 'b', 'bctrl'): break
        if m and m.group(1) in regs and m.group(1) not in vals:
            vals[m.group(1)] = '%s %s' % (mn, ops)
        a -= 4
    print('%08X in %-14s %s' % (s, db.name(f), ' | '.join('%s' % (vals.get(r, r + '=?')) for r in regs)))
