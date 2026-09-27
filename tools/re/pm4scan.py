"""Scan every function for 32-bit constants built with lis+ori/addi and classify
PM4 type-3 packet headers (0xC0cc oo00) by opcode. Output: per-opcode function list."""
import re, sys, collections
from disdb import DB
db = DB.load()
OPC = {0x10:'NOP',0x21:'REG_RMW',0x22:'DRAW_INDX',0x23:'VIZ_QUERY',0x25:'SET_STATE',0x26:'WAIT_FOR_IDLE',
0x27:'IM_LOAD',0x2b:'IM_LOAD_IMMEDIATE',0x2c:'IM_STORE',0x2d:'SET_CONSTANT',0x2e:'LOAD_CONSTANT_CONTEXT',0x2f:'LOAD_ALU_CONSTANT',
0x36:'DRAW_INDX_2',0x37:'INDIRECT_BUFFER_PFD',0x3b:'INVALIDATE_STATE',0x3c:'WAIT_REG_MEM',0x3d:'MEM_WRITE',0x3e:'REG_TO_MEM',
0x3f:'INDIRECT_BUFFER',0x45:'COND_WRITE',0x46:'EVENT_WRITE',0x48:'ME_INIT',0x4a:'SET_SHADER_BASES',0x4b:'SET_BIN_BASE_OFFSET',
0x50:'SET_BIN_MASK',0x51:'SET_BIN_SELECT',0x54:'INTERRUPT',0x55:'SET_CONSTANT2',0x56:'SET_SHADER_CONSTANTS',0x58:'EVENT_WRITE_SHD',
0x59:'EVENT_WRITE_EXT',0x5b:'EVENT_WRITE_ZPD',0x5e:'CONTEXT_UPDATE',0x60:'SET_BIN_MASK_LO',0x61:'SET_BIN_MASK_HI',0x62:'SET_BIN_SELECT_LO',0x63:'SET_BIN_SELECT_HI'}
R = re.compile(r'r(\d+),r(\d+),(-?\d+)')
def consts(f):
    hi = {}
    out = []
    for a, (mn, ops) in db.body(f):
        if mn == 'lis':
            m = re.match(r'r(\d+),(-?\d+)', ops)
            if m: hi[m.group(1)] = (int(m.group(2)) & 0xffff) << 16; out.append((a, hi[m.group(1)]))
            continue
        if mn in ('ori', 'addi', 'oris'):
            m = R.match(ops)
            if m and m.group(2) in hi:
                v = hi[m.group(2)]; i = int(m.group(3))
                v = (v | (i & 0xffff)) if mn == 'ori' else ((v + i) & 0xffffffff) if mn == 'addi' else v
                out.append((a, v))
                continue
        if mn == 'li':
            m = re.match(r'r(\d+),(-?\d+)', ops)
            if m: out.append((a, int(m.group(2)) & 0xffffffff))
    return out
if __name__ == '__main__':
    byop = collections.defaultdict(set)
    for f in db.funcs:
        if f in db.names: continue
        for a, v in consts(f):
            if (v >> 30) == 3 and (v & 0xff) == 0 and ((v >> 8) & 0x7f) in OPC and ((v >> 16) & 0x3fff) < 0x800:
                op = (v >> 8) & 0x7f
                byop[op].add((f, v, a))
    for op in sorted(byop):
        fs = collections.defaultdict(list)
        for f, v, a in byop[op]: fs[f].append('%08X@%08X' % (v, a))
        print('== 0x%02X %s: %d funcs' % (op, OPC[op], len(fs)))
        for f in sorted(fs): print('   %s  %s  callers=%d' % (db.name(f), ' '.join(sorted(fs[f])[:4]), len(db.calls().get(f, []))))
