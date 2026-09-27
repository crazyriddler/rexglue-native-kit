import struct
import sys

def convert(src, dst):
    with open(src, "rb") as f:
        data = f.read()

    id_len, cmap_type, img_type = data[0], data[1], data[2]
    width = data[12] | (data[13] << 8)
    height = data[14] | (data[15] << 8)
    bpp = data[16]
    descriptor = data[17]

    if img_type != 2 or bpp != 32 or cmap_type != 0:
        raise ValueError(f"unsupported TGA: type={img_type} bpp={bpp} cmap={cmap_type}")

    pixel_offset = 18 + id_len
    pixel_data = data[pixel_offset:pixel_offset + width * height * 4]
    if len(pixel_data) != width * height * 4:
        raise ValueError("pixel data size mismatch")

    top_to_bottom = bool(descriptor & 0x20)
    if not top_to_bottom:
        # flip rows so the DDS (top-down) matches
        row_size = width * 4
        rows = [pixel_data[i:i+row_size] for i in range(0, len(pixel_data), row_size)]
        pixel_data = b"".join(reversed(rows))

    # DDS_HEADER (legacy, uncompressed A8R8G8B8 - matches TGA's native BGRA byte order)
    DDSD_CAPS = 0x1
    DDSD_HEIGHT = 0x2
    DDSD_WIDTH = 0x4
    DDSD_PITCH = 0x8
    DDSD_PIXELFORMAT = 0x1000
    DDPF_ALPHAPIXELS = 0x1
    DDPF_RGB = 0x40
    DDSCAPS_TEXTURE = 0x1000

    flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT
    pitch = width * 4

    header = b"DDS "
    header += struct.pack("<I", 124)          # header size
    header += struct.pack("<I", flags)
    header += struct.pack("<I", height)
    header += struct.pack("<I", width)
    header += struct.pack("<I", pitch)
    header += struct.pack("<I", 0)            # depth
    header += struct.pack("<I", 0)            # mipmap count
    header += b"\x00" * (11 * 4)              # reserved1
    # DDS_PIXELFORMAT (32 bytes)
    header += struct.pack("<I", 32)           # pixel format size
    header += struct.pack("<I", DDPF_ALPHAPIXELS | DDPF_RGB)
    header += struct.pack("<I", 0)            # fourCC (unused, RGB)
    header += struct.pack("<I", 32)           # RGB bit count
    header += struct.pack("<I", 0x00FF0000)   # R mask
    header += struct.pack("<I", 0x0000FF00)   # G mask
    header += struct.pack("<I", 0x000000FF)   # B mask
    header += struct.pack("<I", 0xFF000000)   # A mask
    header += struct.pack("<I", DDSCAPS_TEXTURE)  # caps
    header += struct.pack("<I", 0)            # caps2
    header += struct.pack("<I", 0)            # caps3
    header += struct.pack("<I", 0)            # caps4
    header += struct.pack("<I", 0)            # reserved2

    assert len(header) == 4 + 4 + 124 - 4  # "DDS " + size field counted once
    with open(dst, "wb") as f:
        f.write(header)
        f.write(pixel_data)

if __name__ == "__main__":
    convert(sys.argv[1], sys.argv[2])
    print("wrote", sys.argv[2])
