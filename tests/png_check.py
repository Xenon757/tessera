"""Verifies the PNGs test_png.cpp writes with Python's own zlib: the signature,
every chunk's CRC-32, the header, and every pixel against png_test.rgb. Checks the
encoder's own choice of row filters, and each of the five filters forced in turn."""
import struct
import sys
import zlib

folder = sys.argv[1]
expected = open(f"{folder}/png_test.rgb", "rb").read()


def check(name):
    data = open(f"{folder}/{name}", "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "bad signature"
    return verify(name, data)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def verify(name, data):
    pos, chunks = 8, []
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        (crc,) = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])
        assert crc == zlib.crc32(kind + body), f"bad CRC on {kind}"
        chunks.append((kind, body))
        pos += 12 + length
    assert [k for k, _ in chunks] == [b"IHDR", b"IDAT", b"IEND"], [k for k, _ in chunks]

    width, height, depth, color, comp, filt, interlace = struct.unpack(">IIBBBBB", chunks[0][1])
    assert (depth, color, comp, filt, interlace) == (8, 2, 0, 0, 0)
    raw = zlib.decompress(chunks[1][1])
    stride = width * 3
    assert len(raw) == height * (stride + 1)

    pixels, prev, used = bytearray(), bytearray(stride), set()
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        used.add(f)
        for x in range(stride):
            a = line[x - 3] if x >= 3 else 0
            b = prev[x]
            c = prev[x - 3] if x >= 3 else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            line[x] = (line[x] + pred) & 0xFF
        pixels += line
        prev = line

    assert bytes(pixels) == expected, "pixels differ"
    print(f"png_check {name}: {width}x{height}, filters used {sorted(used)}, "
          f"{len(data)} bytes vs {len(expected)} raw: ok")
    return used


check("png_test.png")
for f in range(5):
    assert check(f"png_test_f{f}.png") == {f}
