"""Extract an Xbox 360 STFS package (CON/LIVE/PIRS: saves, DLC) into a folder.

Port of the SDK reader (src/filesystem/devices/stfs_container_device.cpp).
usage: stfs_extract.py <package> <out_dir>      -> files + prints metadata
       stfs_extract.py --info <package>          -> metadata only
Also writes <out_dir>/../<name>.header-ready metadata via --header <file>: an
XCONTENT_AGGREGATE_DATA (0x148 bytes) the ReXGlue content manager reads from
content_root/<xuid>/<title>/Headers/<content type>/<file name>.header.
"""
import argparse
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = [0xAA, 0xAA * 0xAA, 0xAA * 0xAA * 0xAA]
END = 0xFFFFFF


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


class Stfs:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        self.magic = d[:4]
        if self.magic not in (b'CON ', b'LIVE', b'PIRS'):
            raise ValueError(f'not an STFS package: {self.magic!r}')
        self.header_size = struct.unpack_from('>I', d, 0x340)[0]
        self.content_type = struct.unpack_from('>I', d, 0x344)[0]
        self.title_id = struct.unpack_from('>I', d, 0x360)[0]
        self.profile_id = struct.unpack_from('>Q', d, 0x371)[0]
        vd = 0x379
        self.flags = d[vd + 2]
        self.ft_count = struct.unpack_from('<H', d, vd + 3)[0]
        self.ft_block = u24le(d, vd + 5)
        self.total_blocks = struct.unpack_from('>I', d, vd + 0x1C)[0]
        self.volume_type = struct.unpack_from('>I', d, 0x3A9)[0]
        self.display_name = d[0x411:0x411 + 0x80].decode('utf-16-be', 'ignore').split('\0')[0]
        self.read_only = self.flags & 1
        self.root_active = (self.flags >> 1) & 1
        self.bpht = 1 if self.read_only else 2
        self.step0 = PER_LEVEL[0] + self.bpht
        self.step1 = PER_LEVEL[1] + (PER_LEVEL[0] + 1) * self.bpht
        self.base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)
        self.cache = {}

    def block_offset(self, bi):
        base = PER_LEVEL[0]
        block = bi
        for _ in range(3):
            block += ((bi + base) // base) * self.bpht
            if bi < base:
                break
            base *= PER_LEVEL[0]
        return self.base + (block << 12)

    def hash_block_number(self, bi, level):
        if level == 0:
            if bi < PER_LEVEL[0]:
                return 0
            block = (bi // PER_LEVEL[0]) * self.step0
            block += ((bi // PER_LEVEL[1]) + 1) * self.bpht
            return block if bi < PER_LEVEL[1] else block + self.bpht
        if level == 1:
            if bi < PER_LEVEL[1]:
                return self.step0
            return (bi // PER_LEVEL[1]) * self.step1 + self.bpht
        return self.step1

    def hash_offset(self, bi, level):
        return self.base + (self.hash_block_number(bi, level) << 12)

    def table(self, off):
        if off not in self.cache:
            self.cache[off] = self.d[off:off + BLOCK]
        return self.cache[off]

    def entry_info(self, tbl, rec):
        return struct.unpack_from('>I', tbl, rec * 0x18 + 0x14)[0]

    def next_block(self, bi):
        sec = BLOCK if self.root_active else 0
        if self.read_only:
            sec = 0
        else:
            if self.total_blocks > PER_LEVEL[0]:
                if self.total_blocks > PER_LEVEL[1]:
                    t2 = self.table(self.hash_offset(bi, 2) + sec)
                    info = self.entry_info(t2, (bi // PER_LEVEL[1]) % PER_LEVEL[0])
                    sec = BLOCK if info & 0x40000000 else 0
                t1 = self.table(self.hash_offset(bi, 1) + sec)
                info = self.entry_info(t1, (bi // PER_LEVEL[0]) % PER_LEVEL[0])
                sec = BLOCK if info & 0x40000000 else 0
        t0 = self.table(self.hash_offset(bi, 0) + sec)
        return self.entry_info(t0, bi % PER_LEVEL[0]) & 0xFFFFFF

    def files(self):
        """Yields (path, data or None for directories)."""
        entries = []
        tb = self.ft_block
        for _ in range(self.ft_count):
            off = self.block_offset(tb)
            for m in range(BLOCK // 0x40):
                e = self.d[off + m * 0x40:off + (m + 1) * 0x40]
                if e[0] == 0:
                    break
                flags = e[0x28]
                name = e[:flags & 0x3F].decode('latin1')
                is_dir = bool(flags & 0x80)
                start = u24le(e, 0x2F)
                parent = struct.unpack_from('>H', e, 0x32)[0]
                length = struct.unpack_from('>I', e, 0x34)[0]
                path = name if parent == 0xFFFF else entries[parent][0] + '/' + name
                entries.append((path, is_dir))
                if is_dir:
                    yield path, None
                    continue
                data = bytearray()
                bi, remaining = start, length
                while remaining and bi != END:
                    n = min(BLOCK, remaining)
                    o = self.block_offset(bi)
                    data += self.d[o:o + n]
                    remaining -= n
                    bi = self.next_block(bi)
                if remaining:
                    raise ValueError(f'{path}: {remaining} bytes missing (broken chain)')
                yield path, bytes(data)
            tb = self.next_block(tb)
            if tb == END:
                break

    def content_header(self, file_name):
        """XCONTENT_AGGREGATE_DATA (0x148 bytes, big endian)."""
        name = self.display_name.encode('utf-16-be')[:254]
        out = struct.pack('>II', 1, self.content_type)
        out += name.ljust(256, b'\0')
        out += file_name.encode('latin1')[:42].ljust(42, b'\0') + b'\0\0'
        out += bytes(4)  # align the u64 xuid to 8 bytes
        out += struct.pack('>QI', 0, self.title_id) + bytes(4)
        assert len(out) == 0x148
        return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--info', action='store_true')
    ap.add_argument('--header', help='also write an XCONTENT header file here')
    ap.add_argument('--file-name', help='content file name for --header (default: package file name)')
    ap.add_argument('package')
    ap.add_argument('out_dir', nargs='?')
    a = ap.parse_args()
    sys.stdout.reconfigure(errors="replace")
    s = Stfs(a.package)
    print(f'{s.magic.decode()} title {s.title_id:08X} type {s.content_type:08X} profile {s.profile_id:016X} '
          f'"{s.display_name}" blocks {s.total_blocks} ro {s.read_only}')
    if a.info:
        for path, data in s.files():
            print(f'  {path} {"<dir>" if data is None else len(data)}')
        return
    if not a.out_dir:
        sys.exit('out_dir required')
    for path, data in s.files():
        dst = os.path.join(a.out_dir, *path.split('/'))
        if data is None:
            os.makedirs(dst, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
            open(dst, 'wb').write(data)
            print(f'  {path} {len(data)}')
    if a.header:
        os.makedirs(os.path.dirname(a.header), exist_ok=True)
        fn = a.file_name or os.path.basename(a.package)
        open(a.header, 'wb').write(s.content_header(fn))


if __name__ == '__main__':
    main()
