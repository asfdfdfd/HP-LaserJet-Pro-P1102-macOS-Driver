#!/usr/bin/env python3
"""Generate test fixtures (PBM/PGM) into the given directory."""
import random
import re
import struct
import sys


def f32(x):
    """Single-precision round-trip, mirroring the filter's float math."""
    return struct.unpack("f", struct.pack("f", x))[0]


def generate(out):
    def p4(name, w, h, rowfn):
        with open(f"{out}/{name}", "wb") as f:
            f.write(b"P4\n%d %d\n" % (w, h))
            bpl = (w + 7) // 8
            for y in range(h):
                f.write(rowfn(y, bpl))

    def p5(name, w, h, rowfn):
        with open(f"{out}/{name}", "wb") as f:
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
    with open(f"{out}/p1.pbm", "rb") as a, open(f"{out}/p2.pbm", "rb") as b, \
            open(f"{out}/two.pbm", "wb") as dst:
        dst.write(a.read())
        dst.write(b.read())

    # 4. Gradient page (auto-halftone should pick diffusion)
    def gradrow(y):
        return bytes((x * 255) // 5100 for x in range(5100))

    p5("grad.pgm", 5100, 6600, gradrow)

    # 5. Grayscale page @ 600x600 (threshold test)
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

    # 6. Imageable-area fixtures (margins): the raster covers only the
    # imageable box, like cgpdftoraster on macOS; the filter must pad it
    # back to the full page.  Offsets replicate the filter's rounding
    # (C round-half-away: int(x + 0.5)).
    def stripes_at(w, h):
        pbm = bytearray(b"P4\n%d %d\n" % (w, h))
        bpl = (w + 7) // 8
        for y in range(h):
            if y < 60 or y >= h - 60:
                pbm += b"\xff" * bpl
            else:
                r = bytearray(bpl)
                for x in range(0, w, 128):
                    r[x // 8] |= 0x80 >> (x % 8)
                pbm += r
        return bytes(pbm)

    def margins_pbm(w, h, img, off_x, off_y, img_h):
        full = bytearray(b"P4\n%d %d\n" % (w, h))
        fbpl = (w + 7) // 8
        ibpl = (len(img) - 13) // img_h
        for y in range(h):
            row = bytearray(fbpl)
            if off_y <= y < off_y + img_h and y - off_y < img_h:
                src = img[13 + (y - off_y) * ibpl:13 + (y - off_y) * ibpl + ibpl]
                for i in range(ibpl):
                    b = src[i]
                    if not b:
                        continue
                    for bit in range(8):
                        if b & (0x80 >> bit):
                            p = off_x + i * 8 + bit
                            if p < fbpl * 8:
                                row[p // 8] |= 0x80 >> (p & 7)
            full += row
        return bytes(full)

    # 600 dpi: imageable 4911x6411; offsets replicate the filter's
    # float32 arithmetic: (unsigned)(f32(v) * res / 72.0 + 0.5)
    img_w, img_h = 4911, 6411
    off_x = int(f32(11.34) * 600 / 72 + 0.5)
    off_y = 6600 - int(f32(780.66) * 600 / 72 + 0.5)
    im600 = stripes_at(img_w, img_h)
    with open(f"{out}/test600_im.pbm", "wb") as f:
        f.write(im600)
    with open(f"{out}/test600_margins.pbm", "wb") as f:
        f.write(margins_pbm(5100, 6600, im600, off_x, off_y, img_h))

    # 1200x600: imageable 9822x6411, off (189, 95)
    img_w, img_h = 9822, 6411
    off_x = int(f32(11.34) * 1200 / 72 + 0.5)
    im1200 = stripes_at(img_w, img_h)
    with open(f"{out}/test1200_im.pbm", "wb") as f:
        f.write(im1200)
    with open(f"{out}/test1200_margins.pbm", "wb") as f:
        f.write(margins_pbm(10200, 6600, im1200, off_x, off_y, img_h))

    # 7. Three pages for the odd-count manual duplex case
    p3 = bytearray(b"P4\n5100 6600\n")
    bpl3 = 638
    for y in range(6600):
        p3 += b"\xff" * bpl3 if (y < 30 or y >= 6600 - 30) else b"\x00" * bpl3
    with open(f"{out}/p3.pbm", "wb") as f:
        f.write(p3)
    with open(f"{out}/two.pbm", "rb") as a, open(f"{out}/p3.pbm", "rb") as b, \
            open(f"{out}/three.pbm", "wb") as dst:
        dst.write(a.read())
        dst.write(b.read())

    # Derived fixtures:
    #  - gray_thr.pbm: threshold of gray.pgm (px < 128 -> black), the
    #    expected output of the filter with Halftone=Threshold
    #  - gray_K.pgm:   gray.pgm stored in CUPS K convention (0=white,
    #    255=black) for the K-8bpp raster test
    src = open(f"{out}/gray.pgm", "rb").read()
    m = re.match(rb"P5\s+(\d+)\s+(\d+)\s+\d+\s*\n", src)
    w, h = int(m.group(1)), int(m.group(2))
    data = src[m.end():]

    thr = bytearray(b"P4\n%d %d\n" % (w, h))
    bpl = (w + 7) // 8
    for y in range(h):
        row = data[y * w:(y + 1) * w]
        r = bytearray(bpl)
        for x in range(w):
            if row[x] < 128:
                r[x // 8] |= 0x80 >> (x & 7)
        thr += r
    with open(f"{out}/gray_thr.pbm", "wb") as f:
        f.write(thr)

    kpgm = bytearray(b"P5\n%d %d\n255\n" % (w, h))
    for y in range(h):
        kpgm += bytes(255 - b for b in data[y * w:(y + 1) * w])
    with open(f"{out}/gray_K.pgm", "wb") as f:
        f.write(kpgm)


if __name__ == "__main__":
    generate(sys.argv[1])
