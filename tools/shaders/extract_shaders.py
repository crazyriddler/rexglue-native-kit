#!/usr/bin/env python3
"""
Extract every Xbox 360 XDK shader container from Conan's game data.

Scans files byte-by-byte (via bytes.find on the container magic) because
1659 of the 1800 containers in shaders/shaders.stx start at file offsets with
offset % 4 == 1, which stock XenosRecomp's 4-byte-stride scan misses.

Outputs (default root: artifacts/shaders):
  raw/<container_hash>.<vs|ps>.bin   the container, exactly virtualSize+physicalSize bytes
  manifest.json                      one record per unique container + all occurrences

Hash definitions (all XXH3_64, seed 0, printed as 16 upper-case hex digits):
  container_hash  XXH3_64(container[0 : virtualSize + physicalSize])
                  == UnleashedRecomp/re:Blue/XenosRecomp key:
                  XXH3_64bits(function, function[1] + function[2])
  ucode_hash      XXH3_64(container[virtualSize + shader.physicalOffset :
                                    ... + shader.size])
                  i.e. the raw big-endian microcode dwords exactly as they sit in
                  guest memory. This is what ReXGlue's Xenos backend hashes:
                  PipelineCache::LoadShader -> XXH3_64bits(host_address,
                  dword_count*4) where host_address points at guest memory
                  (no byte swap) for PM4 IM_LOAD / IM_LOAD_IMMEDIATE.
                  NOTE: see join_runtime_cache.py - vertex shaders are patched by
                  the XDK at bind time (vfetch constants), so VS runtime hashes
                  may not match; the pixel-shader join is exact.
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

try:
    import xxhash
except ImportError:  # pragma: no cover
    sys.exit("python module 'xxhash' missing: bash scripts/setup_toolchain.sh (kit Python) or python -m pip install xxhash")

MAGIC = b"\x10\x2a\x11"
HDR = struct.Struct(">9I")  # flags virtualSize physicalSize fieldC ctabOff defOff shaderOff field1C field20
SHADER_HDR = struct.Struct(">6I")  # physicalOffset size field8 fieldC field10 interpolatorInfo

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from kitcfg import PORT, CFG  # noqa: E402


def h64(b: bytes) -> str:
    return f"{xxhash.xxh3_64_intdigest(b):016X}"


def parse_container(data: bytes, off: int):
    """Return dict describing a container at data[off:], or None if invalid."""
    if off + HDR.size > len(data):
        return None
    (flags, vsize, psize, fieldC, ctab, deft, shoff, f1c, f20) = HDR.unpack_from(data, off)
    if (flags & 0xFFFFFF00) != 0x102A1100 or f1c != 0 or f20 != 0:
        return None
    size = vsize + psize
    if size == 0 or off + size > len(data):
        return None
    # Stronger sanity than XenosRecomp: offsets must land inside the virtual section.
    if not (0 < ctab < vsize and 0 < shoff < vsize and deft < vsize):
        return None
    if shoff + SHADER_HDR.size > vsize:
        return None
    phys_off, ucode_size = SHADER_HDR.unpack_from(data, off + shoff)[:2]
    if ucode_size == 0 or ucode_size % 4 or phys_off + ucode_size > psize:
        return None
    blob = data[off:off + size]
    ucode = blob[vsize + phys_off: vsize + phys_off + ucode_size]
    return {
        "flags": flags,
        "type": "ps" if (flags & 1) == 0 else "vs",
        "virtual_size": vsize,
        "physical_size": psize,
        "size": size,
        "ucode_offset": vsize + phys_off,
        "ucode_size": ucode_size,
        "container_hash": h64(blob),
        "ucode_hash": h64(ucode),
        "blob": blob,
    }


def stx_effect_index(data: bytes):
    """Offsets of '<name>.fxc' effect records in shaders.stx: [(offset, name)].

    Records look like: be_u32 size, 'Name.fxc\\0', ... The 4-byte size before
    the name has a zero top byte, which disambiguates the name start.
    """
    out = []
    for m in re.finditer(rb"[A-Za-z0-9_]+\.fxc\x00", data):
        s = m.start()
        e = m.end() - 1
        # shrink from the left until the preceding u32 has a zero top byte
        while s < e and not (s >= 4 and data[s - 4] == 0):
            s += 1
        out.append((s, data[s:e].decode("ascii", "replace")))
    return out


def scan_file(path: Path, rel: str, found: dict, occurrences: list, stats: dict):
    data = path.read_bytes()
    effects = stx_effect_index(data) if path.suffix.lower() == ".stx" else []
    eff_i = -1
    pos = 0
    count = 0
    while True:
        i = data.find(MAGIC, pos)
        if i < 0:
            break
        c = parse_container(data, i)
        if c is None:
            pos = i + 1
            continue
        while eff_i + 1 < len(effects) and effects[eff_i + 1][0] < i:
            eff_i += 1
        effect = effects[eff_i][1] if eff_i >= 0 else None
        occ = {
            "container_hash": c["container_hash"],
            "file": rel,
            "offset": i,
            "offset_mod4": i % 4,
            "effect": effect,
        }
        occurrences.append(occ)
        rec = found.get(c["container_hash"])
        if rec is None:
            blob = c.pop("blob")
            c["occurrences"] = 0
            c["first_source"] = {"file": rel, "offset": i, "effect": effect}
            c["effects"] = []
            c["_blob"] = blob
            found[c["container_hash"]] = rec = c
        rec["occurrences"] += 1
        if effect and effect not in rec["effects"]:
            rec["effects"].append(effect)
        count += 1
        pos = i + c["size"]  # containers do not overlap
    stats[rel] = count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=str(Path(PORT) / "game"))
    ap.add_argument("--out", default=str(ROOT / "artifacts" / "shaders"))
    ap.add_argument("--no-xex", action="store_true", help="skip decoding/scanning default.xex")
    ap.add_argument("--skip-ext", default=".bik,.fsb,.fev,.wav,.xma",
                    help="comma list of extensions not scanned (media). Use '' to scan everything")
    args = ap.parse_args()

    game = Path(args.game)
    out = Path(args.out)
    raw = out / "raw"
    work = out / "work"
    raw.mkdir(parents=True, exist_ok=True)
    work.mkdir(parents=True, exist_ok=True)
    skip = {e.strip().lower() for e in args.skip_ext.split(",") if e.strip()}

    found, occurrences, stats = {}, [], {}
    scanned = 0
    for dirpath, dirnames, filenames in os.walk(game, followlinks=False):
        # never follow the directory symlinks the port tree contains
        dirnames[:] = sorted(d for d in dirnames if not os.path.islink(os.path.join(dirpath, d)))
        for fn in sorted(filenames):
            p = Path(dirpath) / fn
            if p.is_symlink() or p.suffix.lower() in skip:
                continue
            rel = p.relative_to(game).as_posix()
            scan_file(p, rel, found, occurrences, stats)
            scanned += 1

    xex_note = None
    if not args.no_xex:
        xex = game / "default.xex"
        img = work / "default_image.bin"
        dec = ROOT / "scripts" / "port" / "xex_decode.py"
        if xex.exists() and dec.exists():
            if not img.exists():
                r = subprocess.run([sys.executable, str(dec), str(xex), str(img)], capture_output=True, text=True)
                if r.returncode != 0:
                    xex_note = "xex decode failed: " + (r.stderr.strip()[-300:])
            if img.exists():
                scan_file(img, "default.xex[decoded image]", found, occurrences, stats)
                xex_note = xex_note or "decoded image scanned"

    for h, rec in found.items():
        blob = rec.pop("_blob")
        (raw / f"{h}.{rec['type']}.bin").write_bytes(blob)

    shaders = sorted(found.values(), key=lambda r: (r["type"], r["container_hash"]))
    manifest = {
        "schema": 1,
        "game": f"{CFG.get('GAME_NAME', 'game')} {CFG.get('TITLE_ID', '')}",
        "hash_definitions": {
            "container_hash": "XXH3_64 (seed 0) of container bytes [0, virtualSize+physicalSize); "
                              "same key as XenosRecomp/UnleashedRecomp/re:Blue g_shaderCacheEntries",
            "ucode_hash": "XXH3_64 (seed 0) of microcode bytes [virtualSize+shader.physicalOffset, +shader.size) "
                          "as stored (big-endian dwords); = ReXGlue Xenos Shader::ucode_data_hash for PS "
                          "and for VS before XDK vfetch patching",
        },
        "files_scanned": scanned,
        "containers_per_file": {k: v for k, v in stats.items() if v},
        "xex": xex_note,
        "total_occurrences": len(occurrences),
        "unique_containers": len(shaders),
        "unique_vs": sum(1 for s in shaders if s["type"] == "vs"),
        "unique_ps": sum(1 for s in shaders if s["type"] == "ps"),
        "unique_ucode": len({s["ucode_hash"] for s in shaders}),
        "shaders": shaders,
        "occurrences": occurrences,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    print(f"scanned {scanned} files; {len(occurrences)} containers, {len(shaders)} unique "
          f"({manifest['unique_vs']} VS / {manifest['unique_ps']} PS), {manifest['unique_ucode']} unique ucode; "
          f"per file: {manifest['containers_per_file']}; xex: {xex_note}")


if __name__ == "__main__":
    main()
