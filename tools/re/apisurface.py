"""List D3D-library functions called from game code (functions outside the lib range),
ranked by number of call sites. Lib range(s) given as args or defaults."""
import sys, collections
from disdb import DB
db = DB.load(); c = db.calls()
RANGES = [(0x822DD000, 0x822FB000)]
def inlib(a): return any(lo <= a < hi for lo, hi in RANGES)
rows = []
for f in db.funcs:
    if not inlib(f): continue
    sites = c.get(f, [])
    ext = [s for s in sites if not inlib(db.func_of(s))]
    if ext:
        rows.append((len(ext), f, len({db.func_of(s) for s in ext}), len(sites), db.func_end(f)-f))
for n, f, nf, tot, size in sorted(rows, reverse=True):
    print('%08X ext_sites=%4d ext_funcs=%4d total=%4d size=%5d' % (f, n, nf, tot, size))
