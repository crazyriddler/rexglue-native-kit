"""Quick queries: q.py dis ADDR | callers ADDR | callees ADDR | grep REGEX [limit]"""
import sys, re
from disdb import DB, dump
db = DB.load()
cmd = sys.argv[1]
def A(x): return int(x, 16)
if cmd == 'dis':
    f = db.func_of(A(sys.argv[2])); print('; func', db.name(f), 'end', '%08X' % db.func_end(f)); print(dump(db, f))
elif cmd == 'callers':
    t = A(sys.argv[2]); cs = db.calls().get(t, [])
    fs = {}
    for c in cs: fs.setdefault(db.func_of(c), []).append(c)
    print('%d sites in %d funcs' % (len(cs), len(fs)))
    for f, l in sorted(fs.items()): print(' ', db.name(f), ' '.join('%08X' % x for x in l))
elif cmd == 'callees':
    f = db.func_of(A(sys.argv[2])); c = db.calls()
    for a, (mn, ops) in db.body(f):
        if mn in ('bl', 'b'):
            m = re.match(r'0x([0-9a-f]+)|([0-9a-f]+)', ops)
            t = int(m.group(1) or m.group(2), 16)
            if mn == 'bl' or t in db.names or db.func_of(t) != f:
                if mn == 'b' and db.func_of(t) == f: continue
                print('%08X %s %s (%d callers)' % (a, mn, db.name(t), len(c.get(t, []))))
elif cmd == 'grep':
    r = re.compile(sys.argv[2]); lim = int(sys.argv[3]) if len(sys.argv) > 3 else 200; n = 0
    for a in sorted(db.ins):
        mn, ops = db.ins[a]
        if r.search(mn + ' ' + ops):
            print('%08X %-8s %-30s in %s' % (a, mn, ops, db.name(db.func_of(a)))); n += 1
            if n >= lim: break
