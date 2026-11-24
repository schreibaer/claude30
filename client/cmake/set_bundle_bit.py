#!/usr/bin/env python3
"""Set the Finder's "has bundle" flag in a MacBinary file, so the Finder reads
the app's BNDL and shows its icon. Retro68 writes the flags as zero.

Usage: set_bundle_bit.py file.bin
Fixes the MacBinary II header in place: Finder flags (byte 73, high byte of
fdFlags; kHasBundle is 0x2000) and the header CRC (bytes 124-125).
"""
import sys


def crc16_ccitt(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def main(path):
    with open(path, "r+b") as f:
        header = bytearray(f.read(128))
        if len(header) < 128 or header[0] != 0 or header[74] != 0:
            sys.exit(f"{path}: not a MacBinary file")
        header[73] = (header[73] | 0x20) & ~0x01  # has bundle; not yet "inited"
        crc = crc16_ccitt(header[:124])
        header[124] = crc >> 8
        header[125] = crc & 0xFF
        f.seek(0)
        f.write(header)


if __name__ == "__main__":
    main(sys.argv[1])
