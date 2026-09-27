#!/usr/bin/env python3
"""
Join ReXGlue Xenos-backend shader storage (<cache>/shaders/shareable/<TITLE_ID>.xsh)
against the extracted container corpus (artifacts/shaders/manifest.json).

.xsh format (src/graphics/d3d12/pipeline_cache.cpp, ShaderStoredHeader):
  u32 'XESH' magic (0x48534558 LE), u32 byte-swapped version 0x20201219
  repeated: u64 ucode_data_hash, u32 (ucode_dword_count:31 | type:1 [0=VS,1=PS]),
            ucode_dword_count * u32 raw guest dwords (big-endian, as in memory)

Match levels per runtime shader:
  exact      runtime ucode_data_hash == corpus ucode_hash
  vfetch     only differs from a corpus ucode inside vertex-fetch instruction
             words (XDK patches VS vfetch const index/stride/offset at bind time).
             Reported with the normalized hash 'ucode_hash_novfetch'.
  fuzzy      same type and dword count as a corpus shader and <= 25% of dwords
             differ (XDK bind-time linkage also rewrites vfetch dst regs and
             clears export write masks of interpolators the PS does not read);
             best candidate by fewest differing dwords
  none       no corpus match (XDK-internal shader, e.g. clear/resolve helpers)

Writes artifacts/shaders/runtime_join.json and updates catalog-facing fields
through build_catalog.py (which reads this file).
"""
import argparse
import glob
import json
import struct
import sys
from pathlib import Path

import xxhash

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from kitcfg import CFG  # noqa: E402
TITLE = CFG.get("TITLE_ID", "") or "*"


def read_xsh(path: Path):
    d = path.read_bytes()
    if len(d) < 8 or struct.unpack_from("<I", d, 0)[0] != 0x48534558:
        return []
    ver = struct.unpack_from(">I", d, 4)[0]
    if ver != 0x20201219:
        print(f"warning: {path} version {ver:08X}", file=sys.stderr)
    off, out = 8, []
    while off + 12 <= len(d):
        h, cnt_type = struct.unpack_from("<QI", d, off)
        n = cnt_type & 0x7FFFFFFF
        t = "ps" if (cnt_type >> 31) else "vs"
        off += 12
        u = d[off:off + n * 4]
        if len(u) != n * 4:
            break
        off += n * 4
        if xxhash.xxh3_64_intdigest(u) != h:
            print(f"warning: hash mismatch in {path} at {off}", file=sys.stderr)
            break
        out.append({"ucode_data_hash": f"{h:016X}", "type": t, "dwords": n, "ucode": u})
    return out


def exec_slots(ucode: bytes):
    """Return list of (instr_index, is_fetch, is_vfetch) for every ALU/fetch
    instruction slot referenced by exec control-flow instructions."""
    dw = struct.unpack(f">{len(ucode) // 4}I", ucode)
    size = len(dw)
    slots = {}
    # CF instructions are packed 2 per 3 dwords, until the first exec address.
    limit = size
    i = 0
    cf_index = 0
    while i + 2 < size and i < limit:
        w0, w1, w2 = dw[i], dw[i + 1], dw[i + 2]
        cfs = [(w0 | ((w1 & 0xFFFF) << 32)), ((w1 >> 16) | (w2 << 16))]
        for cf in cfs:
            opcode = (cf >> 44) & 0xF
            # exec-family opcodes (include/rex/graphics/format/ucode.h ControlFlowOpcode):
            # 1 Exec, 2 ExecEnd, 3 CondExec, 4 CondExecEnd, 5 CondExecPred,
            # 6 CondExecPredEnd, 13 CondExecPredClean, 14 CondExecPredCleanEnd
            if opcode in (1, 2, 3, 4, 5, 6, 13, 14):
                address = cf & 0xFFF
                count = (cf >> 12) & 0x7
                sequence = (cf >> 16) & 0xFFF
                if address:
                    limit = min(limit, address * 3)
                for k in range(count):
                    is_fetch = (sequence >> (k * 2)) & 1
                    slots[address + k] = bool(is_fetch)
            cf_index += 1
        i += 3
    return dw, slots


def normalize_vfetch(ucode: bytes) -> bytes:
    """Blank the parts of vertex-fetch instructions the XDK rewrites when it binds
    a vertex declaration to a VS (fetch constant slot, format, stride, offset,
    swizzle). Layout (ucode.h VertexFetchInstruction): word0 bits 0-4 opcode
    (0 = vfetch), 5-10 src, 11 am, 12-17 dst, 18 am, 19 must_be_one,
    20-24 const_index, 25-26 const_index_sel; word1 dst_swiz/format/...;
    word2 stride/offset/pred. Keep word0 bits 0-18, zero the rest."""
    dw, slots = exec_slots(ucode)
    dw = list(dw)
    for idx, is_fetch in slots.items():
        if not is_fetch:
            continue
        b = idx * 3
        if b + 2 >= len(dw):
            continue
        if (dw[b] & 0x1F) == 0:  # vertex fetch
            dw[b] &= 0x7FFFF
            dw[b + 1] = 0
            dw[b + 2] = 0
    return struct.pack(f">{len(dw)}I", *dw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=str(ROOT / "artifacts" / "shaders"))
    ap.add_argument("xsh", nargs="*", help=".xsh files (default: every bench/**/<TITLE_ID>.xsh)")
    args = ap.parse_args()
    corpus = Path(args.corpus)
    manifest = json.loads((corpus / "manifest.json").read_text())

    by_ucode, by_norm, by_len = {}, {}, {}
    for s in manifest["shaders"]:
        blob = (corpus / "raw" / f"{s['container_hash']}.{s['type']}.bin").read_bytes()
        u = blob[s["ucode_offset"]: s["ucode_offset"] + s["ucode_size"]]
        by_ucode.setdefault(s["ucode_hash"], []).append(s["container_hash"])
        nh = f"{xxhash.xxh3_64_intdigest(normalize_vfetch(u)):016X}" if s["type"] == "vs" else s["ucode_hash"]
        s["ucode_hash_novfetch"] = nh
        by_norm.setdefault((s["type"], nh), []).append(s["container_hash"])
        by_len.setdefault((s["type"], len(u)), []).append((s["container_hash"], u))

    files = [Path(p) for p in args.xsh] or sorted(
        Path(p) for p in glob.glob(str(ROOT / "bench" / "**" / f"{TITLE}.xsh"), recursive=True))
    runtime = {}
    for f in files:
        for e in read_xsh(f):
            r = runtime.setdefault(e["ucode_data_hash"], e)
            r.setdefault("sources", []).append(f.relative_to(ROOT).as_posix() if f.is_relative_to(ROOT) else str(f))

    results = []
    for h, e in sorted(runtime.items()):
        rec = {"ucode_data_hash": h, "type": e["type"], "dwords": e["dwords"], "sources": sorted(set(e["sources"]))}
        if h in by_ucode:
            rec["match"] = "exact"
            rec["container_hashes"] = by_ucode[h]
        else:
            nh = f"{xxhash.xxh3_64_intdigest(normalize_vfetch(e['ucode'])):016X}" if e["type"] == "vs" else h
            cands = by_norm.get((e["type"], nh), [])
            rec["match"] = "vfetch" if cands else "none"
            rec["ucode_hash_novfetch"] = nh
            rec["container_hashes"] = cands
            if not cands:
                best = None
                n = len(e["ucode"]) // 4
                a = struct.unpack(f">{n}I", e["ucode"])
                for ch, u in by_len.get((e["type"], len(e["ucode"])), []):
                    b = struct.unpack(f">{n}I", u)
                    d = sum(1 for x, y in zip(a, b) if x != y)
                    if best is None or d < best[1]:
                        best = (ch, d)
                if best and best[1] <= max(1, n // 4):
                    rec["match"] = "fuzzy"
                    rec["container_hashes"] = [best[0]]
                    rec["differing_dwords"] = best[1]
        results.append(rec)

    summary = {}
    for r in results:
        summary.setdefault(f"{r['type']}_{r['match']}", 0)
        summary[f"{r['type']}_{r['match']}"] += 1
    out = {"xsh_files": [str(f) for f in files], "summary": summary,
           "novfetch_by_container": {s["container_hash"]: s["ucode_hash_novfetch"] for s in manifest["shaders"]},
           "runtime_shaders": results}
    (corpus / "runtime_join.json").write_text(json.dumps(out, indent=1))
    print(f"{len(runtime)} runtime shaders from {len(files)} .xsh files: {summary}")


if __name__ == "__main__":
    main()
