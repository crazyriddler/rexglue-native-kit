"""Find tables of function pointers (runs of >= N consecutive big-endian words that are
known function starts) in the image. Optionally restrict pointed-to range."""
import sys, struct
from disdb import DB, BASE
db = DB.load(); img = db.image(); fs = set(db.funcs)
N = int(sys.argv[1]) if len(sys.argv) > 1 else 8
lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0xffffffff
run = []
def flush():
    if len(run) >= N and any(lo <= v < hi for a, v in run):
        print('table @%08X len=%d first=%s' % (run[0][0], len(run), ' '.join('%08X' % v for a, v in run[:6])))
for o in range(0, len(img) - 3, 4):
    v = struct.unpack_from('>I', img, o)[0]
    if v in fs: run.append((BASE + o, v))
    else:
        flush(); run = []
flush()
