#!/usr/bin/env python3
"""Writes a corpus of zlib streams made by Python's own zlib (an independent
reference encoder) for tests/test_zlib.cpp. Covers every DEFLATE block type:
level 0 (stored), Z_FIXED (fixed Huffman), the usual levels (dynamic Huffman),
plus RLE/Huffman-only strategies and multi-block streams with flushes.

Usage: zlib_corpus.py <out.bin>
Format: u32 count, then per case: u32 nameLen, u32 rawLen, u32 compLen, name, raw, comp."""
import random
import struct
import sys
import zlib

random.seed(1234)
cases = []


def add(name, data):
    for level in (0, 1, 6, 9):
        cases.append((f"{name}/L{level}", data, zlib.compress(data, level)))
    for strategy, sname in ((zlib.Z_FIXED, "fixed"), (zlib.Z_RLE, "rle"), (zlib.Z_HUFFMAN_ONLY, "huffonly")):
        c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, strategy)
        cases.append((f"{name}/{sname}", data, c.compress(data) + c.flush()))


add("empty", b"")
add("one", b"x")
add("zeros256k", bytes(1 << 18))
add("random32k", bytes(random.getrandbits(8) for _ in range(32768)))
add("text", b"The quick brown fox jumps over the lazy dog. " * 1000)
add("floats", b"".join(struct.pack("<d", (i % 977) * 0.125 - 30.0) for i in range(10000)))  # like FBX vertices
add("indices", b"".join(struct.pack("<i", i if i % 3 else -i - 1) for i in range(10000)))  # like PolygonVertexIndex
for n in range(60):
    size = random.randint(0, 3000)
    alphabet = random.randint(1, 256)
    runs = random.random() < 0.5
    out = bytearray()
    while len(out) < size:
        out += bytes([random.randrange(alphabet)]) * (random.randint(1, 40) if runs else 1)
    add(f"rand{n}", bytes(out[:size]))
c = zlib.compressobj(6)
data = b""
stream = b""
for i in range(30):
    chunk = bytes(random.getrandbits(8) % 7 for _ in range(1000))
    data += chunk
    stream += c.compress(chunk) + c.flush(zlib.Z_SYNC_FLUSH if i % 2 else zlib.Z_FULL_FLUSH)
cases.append(("multiblock", data, stream + c.flush()))

with open(sys.argv[1], "wb") as f:
    f.write(struct.pack("<I", len(cases)))
    for name, raw, comp in cases:
        nb = name.encode()
        f.write(struct.pack("<III", len(nb), len(raw), len(comp)))
        f.write(nb)
        f.write(raw)
        f.write(comp)
