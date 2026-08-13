#!/usr/bin/env python3
"""Generate test fixtures (PBM/PGM) into the given directory."""
import random
import sys

OUT = sys.argv[1]


def p4(name, w, h, rowfn):
    with open(f"{OUT}/{name}", "wb") as f:
        f.write(b"P4\n%d %d\n" % (w, h))
        bpl = (w + 7) // 8
        for y in range(h):
            f.write(rowfn(y, bpl))


def p5(name, w, h, rowfn):
    with open(f"{OUT}/{name}", "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (w, h))
        for y in range(h):
            f.write(rowfn(y))


# 1. Striped letter page @ 600x600
def stripes(y, bpl):
    if y < 60 or y >= 6600 - 60:
        return b"\xff" * bpl
    r = bytearray(bpl)
    for x in range(0, 5100, 128):
        r[x // 8] |= 0x80 >> (x % 8)
    return bytes(r)


p4("test600.pbm", 5100, 6600, stripes)

# 2. Noise page @ 1200x600 (hard for the compressor)
rng = random.Random(42)


def noise(y, bpl):
    if y < 60 or y >= 6600 - 60:
        return b"\xff" * bpl
    if y % 1000 == 500:
        r = bytearray(bpl)
        for x in range(0, 10200, 8):
            r[x // 8] |= 0x80 >> (x % 8)
        return bytes(r)
    return rng.randbytes(bpl)


p4("testnoise.pbm", 10200, 6600, noise)

# 3. Two simple pages (multi-page stream)
def border(y, bpl):
    return b"\xff" * bpl if (y < 30 or y >= 6600 - 30) else b"\x00" * bpl


p4("p1.pbm", 5100, 6600, border)
p4("p2.pbm", 5100, 6600, border)
with open(f"{OUT}/p1.pbm", "rb") as a, open(f"{OUT}/p2.pbm", "rb") as b, \
        open(f"{OUT}/two.pbm", "wb") as out:
    out.write(a.read())
    out.write(b.read())

# 4. Grayscale page @ 600x600 (threshold test)
def gray(y):
    r = bytearray()
    for x in range(5100):
        if x < 100:
            v = 0
        elif y % 1000 == 500:
            v = 127
        else:
            v = 200
        r.append(v)
    return bytes(r)


p5("gray.pgm", 5100, 6600, gray)
