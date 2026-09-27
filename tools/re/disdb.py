"""Build/load a compact index of the game's PPC disassembly.

Usage (library):  from disdb import DB; db = DB.load()
Inputs (paths from kit.env via tools/kitcfg.py):
  $PORT_DIR/logs/default_full.dis   objdump of the decoded image:
      tools/binutils/powerpc-none-elf-objdump.exe -D -b binary -m powerpc -EB
          --adjust-vma=0x82000000 default_image.bin > default_full.dis
  $PORT_DIR/logs/default_image.bin  decoded image (scripts/port/xex_decode.py or
                                     --dump_xex_image=<file>)
  $PORT_DIR/generated/default/<GAME_NAME>_register.cpp  function starts (codegen)
Cache: $PORT_DIR/logs/disdb.pkl (delete it after re-running codegen).
"""
import os, re, pickle, bisect, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from kitcfg import PORT, GAME
DIS = os.path.join(PORT, 'logs', 'default_full.dis')
REG = os.path.join(PORT, 'generated', 'default', GAME + '_register.cpp')
IMG = os.path.join(PORT, 'logs', 'default_image.bin')
CACHE = os.path.join(PORT, 'logs', 'disdb.pkl')
LINE = re.compile(r'^([0-9a-f]{8}):\t([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}) \t?(\S*)\s*(.*)$')
BASE = 0x82000000
# First code address (REX_CODE_BASE in generated/default/*_pch.h / *_init.h).
CODE_START = int(os.environ.get('CODE_START', '0x82000000'), 16)

class DB:
    def __init__(s):
        s.ins = {}      # addr -> (mn, ops)
        s.funcs = []    # sorted starts
        s.names = {}    # addr -> import name
    @staticmethod
    def build():
        db = DB()
        with open(DIS, 'r', errors='replace') as f:
            for ln in f:
                m = LINE.match(ln)
                if not m: continue
                a = int(m.group(1), 16)
                if a < CODE_START: continue
                db.ins[a] = (m.group(6), m.group(7))
        fs = set()
        for ln in open(REG):
            m = re.search(r'SetFunction\(0x([0-9A-F]+),\s*(\w+)\)', ln)
            if m:
                a = int(m.group(1), 16); fs.add(a)
                if not m.group(2).startswith('sub_'): db.names[a] = m.group(2).replace('__imp__','')
        db.funcs = sorted(fs)
        pickle.dump(db.__dict__, open(CACHE, 'wb'), protocol=4)
        return db
    @staticmethod
    def load():
        if not os.path.exists(CACHE): return DB.build()
        db = DB(); db.__dict__.update(pickle.load(open(CACHE, 'rb'))); return db
    def func_of(s, a):
        i = bisect.bisect_right(s.funcs, a) - 1
        return s.funcs[i] if i >= 0 else None
    def func_end(s, f):
        i = bisect.bisect_right(s.funcs, f)
        return s.funcs[i] if i < len(s.funcs) else f + 4
    def body(s, f):
        e = s.func_end(f); a = f
        while a < e:
            if a in s.ins: yield a, s.ins[a]
            a += 4
    def name(s, a):
        return s.names.get(a, 'sub_%08X' % a)
    def calls(s):
        """returns dict callee -> list of call sites (bl and tail b to function starts)"""
        if hasattr(s, '_calls'): return s._calls
        fset = set(s.funcs); c = {}
        for a, (mn, ops) in s.ins.items():
            if mn in ('bl', 'b'):
                m = re.match(r'0x([0-9a-f]+)', ops) or re.match(r'([0-9a-f]+)', ops)
                if not m: continue
                t = int(m.group(1), 16)
                if mn == 'b' and (t not in fset or s.func_of(a) == t): continue
                c.setdefault(t, []).append(a)
        s._calls = c; return c
    def image(s):
        if not hasattr(s, '_img'): s._img = open(IMG, 'rb').read()
        return s._img
    def u32(s, a):
        return struct.unpack('>I', s.image()[a-BASE:a-BASE+4])[0]
    def cstr(s, a):
        b = s.image(); o = a - BASE; e = b.index(b'\0', o); return b[o:e].decode('latin1')

def dump(db, f, out=None):
    lines = []
    for a, (mn, ops) in db.body(f):
        lines.append('%08X  %-8s %s' % (a, mn, ops))
    return '\n'.join(lines)

if __name__ == '__main__':
    import sys
    db = DB.build() if 'build' in sys.argv else DB.load()
    print(len(db.ins), 'ins', len(db.funcs), 'funcs')
