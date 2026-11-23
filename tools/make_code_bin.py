"""Make recomp/gauntlet_code.bin, the code image N64Recomp and RSPRecomp read, from the game's ROM.

The game keeps its code zlib-compressed in the ROM; the boot loader's segment table (ROM 0x12E0) lists
each segment. This inflates the game segment and the three overlays and lays them out from 32 MB on
(the addresses recomp/gauntlet.syms.toml gives them), so they never overlap the real 16 MB ROM the
runtime reads data from.

usage: make_code_bin.py <rom.z64> <out.bin>
"""
import hashlib
import struct
import sys
import zlib

TABLE = 0x12E0
ENTRIES = 6
ROM_BASE = 0x2000000
ORDER = ('game', 'Transitions', 'Tower', 'Boss')   # the order of the sections in gauntlet.syms.toml
SHA256 = '6da2332854d32f37c60615cbc2b5204f79cc6152afb1811caa23400b29122f98'


def segments(rom):
    segs = {}
    for i in range(ENTRIES):
        e = TABLE + i * 0x30
        name = rom[e:e + 16].split(b'\0')[0].decode('ascii', 'replace')
        rom_off, rom_size = struct.unpack('>2I', rom[e + 16:e + 24])
        try:
            segs[name] = zlib.decompressobj(-15).decompress(rom[rom_off:rom_off + rom_size])
        except zlib.error:
            pass   # $ROMDISK is a header, not code
    return segs


def make(rom):
    segs = segments(rom)
    out = bytearray(ROM_BASE)
    for name in ORDER:
        out += segs[name]
        out += b'\0' * ((-len(out)) & 0xF)
    return bytes(out)


def main():
    with open(sys.argv[1], 'rb') as f:
        rom = f.read()
    data = make(rom)
    got = hashlib.sha256(data).hexdigest()
    if got != SHA256:
        sys.exit(f'the code image does not match the expected one (sha256 {got}): is this the USA ROM?')
    with open(sys.argv[2], 'wb') as f:
        f.write(data)


if __name__ == '__main__':
    main()
