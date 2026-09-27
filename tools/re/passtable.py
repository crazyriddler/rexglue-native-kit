"""Dump the game's render-pass table ({enabled, name*, fn*, category} x N, 16-byte stride)
starting at 0x82A25A70 (found by pointer scan; see RENDERER_ANALYSIS.md)."""
import struct
from disdb import DB, BASE
db = DB.load(); img = db.image(); c = db.calls()
def rows(start=0x82A25A70):
    a = start
    while True:
        en, nm, fn, cat = struct.unpack_from('>IIII', img, a - BASE)
        if not (0x82000000 <= nm < 0x82B00000) or en > 1: break
        yield a, en, db.cstr(nm), fn, cat
        a += 16
if __name__ == '__main__':
    # scan back to find the table start
    s = 0x82A25A70
    while True:
        en, nm, fn, cat = struct.unpack_from('>IIII', img, s - 16 - BASE)
        if en <= 1 and 0x82000000 <= nm < 0x82B00000 and (fn == 0 or 0x821D0000 <= fn < 0x82A00000): s -= 16
        else: break
    for i, (a, en, n, fn, cat) in enumerate(rows(s)):
        callees = []
        if fn and fn in db.ins:
            for x, (mn, ops) in db.body(db.func_of(fn)):
                if mn in ('bl', 'b') and ops.startswith('0x'):
                    t = int(ops.split()[0], 16)
                    if t != db.func_of(fn) and not (x > fn and db.func_of(t) == db.func_of(fn)): callees.append('%08X' % t)
        print('%2d %08X en=%d cat=%d fn=%08X %-28s -> %s' % (i, a, en, cat, fn, n, ' '.join(callees[:8])))
