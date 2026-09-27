import sys
from disdb import DB
db = DB.load(); lo, hi = int(sys.argv[1],16), int(sys.argv[2],16); c = db.calls()
for f in db.funcs:
    if lo <= f < hi:
        cs = c.get(f, []); cf = {db.func_of(x) for x in cs}
        ncallee = sum(1 for a,(mn,o) in db.body(f) if mn=='bl')
        ext = sum(1 for x in cf if not (lo <= x < hi))
        print('%08X size=%5d callers=%3d(funcs %3d, outside %3d) bl=%3d' % (f, db.func_end(f)-f, len(cs), len(cf), ext, ncallee))
