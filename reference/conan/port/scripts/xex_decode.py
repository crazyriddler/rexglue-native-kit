#!/usr/bin/env python3
"""
Decrypt + decompress a "basic compression" XEX2 image into a flat raw memory
dump for external disassembly (objdump), so UnresolvedCall targets can be
inspected with real evidence instead of guessing.

This mirrors exactly what ReXGlue's own src/system/xex_module.cpp does at
runtime (XexModule::ReadImage / DecompressXexImage) — same retail AES-128-CBC
key (public, documented Xbox 360 XEX2 format constant, used here purely to
analyze a legally-owned game file the same way the SDK itself already does
internally), same "basic compression" zero-fill block table. Only handles
compression_type=1 (basic) / encryption_type=1 (normal) — this title's exact
combination, decoded from its XEX_HEADER_FILE_FORMAT_INFO header.
"""
import struct
import sys
from Crypto.Cipher import AES  # noqa: will fall back if unavailable


RETAIL_KEY = bytes([0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
                    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91])


def aes_cbc_zero_iv_decrypt(key: bytes, data: bytes) -> bytes:
    cipher = AES.new(key, AES.MODE_CBC, iv=b"\x00" * 16)
    return cipher.decrypt(data)


def main():
    xex_path = sys.argv[1]
    out_path = sys.argv[2]

    with open(xex_path, "rb") as f:
        data = f.read()

    assert data[0:4] == b"XEX2"
    module_flags, exe_offset, reserved, security_offset, header_count = struct.unpack_from(
        ">IIIII", data, 4)

    opt_headers = {}
    off = 24
    for _ in range(header_count):
        key, value = struct.unpack_from(">II", data, off)
        opt_headers[key] = value
        off += 8

    ffi_off = opt_headers[0x000003FF]
    info_size = struct.unpack_from(">I", data, ffi_off)[0]
    encryption_type, compression_type = struct.unpack_from(">HH", data, ffi_off + 4)
    print(f"encryption_type={encryption_type} compression_type={compression_type}")
    assert encryption_type == 1 and compression_type == 1, (
        "This script only handles basic-compressed, normally-encrypted XEX2 images; "
        "extend it before trusting it on a different title.")

    blocks = []
    boff = ffi_off + 8
    while boff < ffi_off + info_size:
        data_size, zero_size = struct.unpack_from(">II", data, boff)
        if data_size == 0 and zero_size == 0:
            break
        blocks.append((data_size, zero_size))
        boff += 8
    print("basic-compression blocks:", blocks)

    aes_key_encrypted = data[security_offset + 0x150: security_offset + 0x160]
    session_key = aes_cbc_zero_iv_decrypt(RETAIL_KEY, aes_key_encrypted)
    print("session key:", session_key.hex())

    load_address = struct.unpack_from(">I", data, security_offset + 0x110)[0]
    image_size = struct.unpack_from(">I", data, security_offset + 0x4)[0]
    print(f"load_address=0x{load_address:08X} image_size=0x{image_size:X}")

    encrypted_body = data[exe_offset:]
    # Round down to a whole number of AES blocks.
    usable_len = (len(encrypted_body) // 16) * 16
    decrypted = aes_cbc_zero_iv_decrypt(session_key, encrypted_body[:usable_len])

    out = bytearray()
    pos = 0
    for data_size, zero_size in blocks:
        out += decrypted[pos:pos + data_size]
        out += b"\x00" * zero_size
        pos += data_size

    print(f"reconstructed image: {len(out)} bytes (image_size header says 0x{image_size:X})")

    with open(out_path, "wb") as f:
        f.write(out)
    print(f"wrote {out_path}")
    print(f"load address for objdump --adjust-vma: 0x{load_address:08X}")


if __name__ == "__main__":
    main()
