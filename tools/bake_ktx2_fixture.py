#!/usr/bin/env python3
"""Bakes packages/base/textures/demo_bc7.ktx2: an 8x8, 2x2-block-checker
BC7 fixture, hand-encoded with BC7 mode 6 (no partitions, 8-bit-exact
RGBA endpoints via a 7-bit value plus a shared parity bit per endpoint,
per-pixel index). Each block is a solid color, so both endpoints of a
block are set equal and every index is 0 - trivial and exact, no real
BC7 encoder needed for this one small fixture.

This project has no BC7 encoder dependency and isn't adding one; this
script exists only to produce the one committed .ktx2 fixture asset, not
as part of the runtime or build. See src/renderer/Ktx2.h for the reader
this file is meant to round-trip through, and the wiki's Packages page
for why a real cook step lives outside this repo.
"""

import struct
from pathlib import Path

BLOCKS_WIDE = 2
BLOCKS_TALL = 2
WIDTH = BLOCKS_WIDE * 4
HEIGHT = BLOCKS_TALL * 4

# Both all-even RGBA so a shared parity bit of 0 reconstructs them exactly
# via BC7 mode 6's (7-bit value << 1) | pbit expansion.
COLOR_A = (216, 88, 40, 254)
COLOR_B = (40, 120, 216, 254)


class BitWriter:
    """LSB-first bit packer, matching BC7's block bit order."""

    def __init__(self):
        self.bits = []

    def write(self, value, num_bits):
        for i in range(num_bits):
            self.bits.append((value >> i) & 1)

    def to_bytes(self, total_bits):
        while len(self.bits) < total_bits:
            self.bits.append(0)
        assert len(self.bits) == total_bits
        out = bytearray(total_bits // 8)
        for i, bit in enumerate(self.bits):
            if bit:
                out[i // 8] |= 1 << (i % 8)
        return bytes(out)


def encode_endpoint(color):
    """Mode 6: 7 explicit bits + 1 shared parity bit per component,
    8 bits total (exact for RGBA8). Requires every component to share
    the same LSB."""
    parities = {c & 1 for c in color}
    if len(parities) != 1:
        raise ValueError(f"color {color} components don't share one parity bit")
    p = parities.pop()
    sevenBit = tuple(c >> 1 for c in color)
    return sevenBit, p


def encode_solid_block_mode6(color):
    """A solid-color BC7 mode 6 block: both endpoints equal, every index 0."""
    (r, g, b, a), p = encode_endpoint(color)
    w = BitWriter()
    w.write(0b1000000, 7)  # mode 6: six 0 bits then a 1 bit, LSB first
    w.write(r, 7)
    w.write(r, 7)
    w.write(g, 7)
    w.write(g, 7)
    w.write(b, 7)
    w.write(b, 7)
    w.write(a, 7)
    w.write(a, 7)
    w.write(p, 1)
    w.write(p, 1)
    # 16 pixel indices, 4 bits each (first pixel's top bit is implied 0
    # per the BC7 spec, but a value of 0 is representable in 3 bits
    # anyway, so writing 3 then 4-bit zeros for the rest is exact).
    w.write(0, 3)
    for _ in range(15):
        w.write(0, 4)
    return w.to_bytes(128)


def build_level_data():
    grid = [[COLOR_A, COLOR_B], [COLOR_B, COLOR_A]]  # checker at block granularity
    data = bytearray()
    for by in range(BLOCKS_TALL):
        for bx in range(BLOCKS_WIDE):
            data += encode_solid_block_mode6(grid[by][bx])
    return bytes(data)


def build_ktx2(level_data: bytes) -> bytes:
    identifier = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])

    VK_FORMAT_BC7_UNORM_BLOCK = 145
    header = struct.pack(
        "<9I",
        VK_FORMAT_BC7_UNORM_BLOCK,
        1,  # typeSize: 1 for block-compressed
        WIDTH,
        HEIGHT,
        0,  # pixelDepth: not a 3D texture
        0,  # layerCount: not an array texture
        1,  # faceCount: not a cubemap
        1,  # levelCount: no mip chain
        0,  # supercompressionScheme: none
    )

    # dfd/kvd/sgd all empty: this fixture's only consumer is this
    # project's own reader, which never parses them. Not conformant KTX2
    # (a real file always carries a DFD); deliberate for a fixture this
    # narrow in scope - see Ktx2.h.
    index = struct.pack("<4I2Q", 0, 0, 0, 0, 0, 0)

    level_index_placeholder = struct.pack("<3Q", 0, 0, 0)
    level_offset = len(identifier) + len(header) + len(index) + len(level_index_placeholder)
    level_index = struct.pack("<3Q", level_offset, len(level_data), len(level_data))

    return identifier + header + index + level_index + level_data


def main():
    level_data = build_level_data()
    ktx2_bytes = build_ktx2(level_data)
    out_path = Path(__file__).resolve().parent.parent / "packages" / "base" / "textures" / "demo_bc7.ktx2"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(ktx2_bytes)
    print(f"wrote {out_path} ({len(ktx2_bytes)} bytes, {WIDTH}x{HEIGHT} BC7)")


if __name__ == "__main__":
    main()
