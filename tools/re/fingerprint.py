"""Per-function fingerprint: fields of arg0 (r3 'this') loaded/stored, callees, ring writes.
fingerprint.py LO HI [--ext-only]"""
import sys, re, collections
from disdb import DB
db = DB.load(); calls = db.calls()
MEM = re.compile(r'^(r\d+|f\d+|v\d+),(-?\d+)\((r\d+)\)$')
def fp(f):
    this = {'r3'}; ld = set(); st = set(); cl = []; ring = 0; imms = set()
    for a, (mn, ops) in db.body(f):
        m = re.match(r'^(r\d+),(r\d+)$', ops)
        if mn == 'mr' and m:
            if m.group(2) in this: this.add(m.group(1))
            else: this.discard(m.group(1))
            continue
        m = MEM.match(ops)
        if m and m.group(3) in this:
            off = int(m.group(2))
            (st if mn.startswith('st') else ld).add(off)
            if mn.startswith('l') and m.group(1) in this: this.discard(m.group(1))
            continue
        if mn == 'stwu' and m: ring += 1
        if mn == 'bl':
            t = int(ops.split()[0], 16); cl.append(db.name(t))
            this.discard('r3') if 'r3' in this and False else None
        m2 = re.match(r'^(r\d+),', ops)
        if m2 and m2.group(1) in this and not mn.startswith(('st', 'cmp')) and mn != 'mr':
            this.discard(m2.group(1))
    return ld, st, cl, ring
if __name__ == '__main__':
    lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
    ext = '--ext-only' in sys.argv
    inl = lambda a: lo <= a < hi
    for f in db.funcs:
        if not inl(f): continue
        sites = calls.get(f, [])
        e = [s for s in sites if not (0x822DD000 <= db.func_of(s) < 0x822FA000)]
        if ext and not e: continue
        ld, st, cl, ring = fp(f)
        print('%08X sz=%d ext=%d tot=%d ring=%d\n   ST %s\n   LD %s\n   CALL %s' % (f, db.func_end(f)-f, len(e), len(sites), ring,
              ','.join(str(x) for x in sorted(st))[:300], ','.join(str(x) for x in sorted(ld))[:300], ' '.join(cl)[:300]))
