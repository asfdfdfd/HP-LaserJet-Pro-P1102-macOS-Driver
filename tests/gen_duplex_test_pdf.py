#!/usr/bin/env python3
"""Generate a duplex test PDF: 4 pages, each with a huge page number,
TOP/BOTTOM labels, an orientation triangle, a center crosshair and a
corner marker, so the user can report per physical sheet:
  - which page is on the front and which on the back,
  - whether the back is upright when flipping the sheet over the top edge,
  - which corner marker each side has.

Usage: gen_duplex_test_pdf.py [out.pdf]   (default: duplex-test.pdf)
Minimal hand-built PDF, standard Helvetica fonts only (no embedding).
"""
import sys

W, H = 612, 792          # Letter, points
OUT = sys.argv[1] if len(sys.argv) > 1 else "duplex-test.pdf"

# Helvetica-Bold digit advance = 0.556 em (all digits).
def center_x(text, size):
    width = 0.556 * size * len(text)
    return round(W / 2 - width / 2, 1)

def page_content(i):
    num = str(i)
    big = f"BT /F1 {200:.0f} Tf 0 0 0 rg {center_x(num, 200)} 396 Td ({num}) Tj ET\n"
    top = f"BT /F2 20 Tf 0 0 0 rg {center_x('TOP', 20)} 690 Td (TOP) Tj ET\n"
    bot = f"BT /F2 20 Tf 0 0 0 rg {center_x('BOTTOM', 20)} 70 Td (BOTTOM) Tj ET\n"
    corner = f"BT /F2 16 Tf 0 0 0 rg 36 748 Td ({num}) Tj ET\n"
    # orientation triangle at top center (apex up) + center crosshair
    tri = "306 632 m 284 588 l 328 588 l h f\n"
    cross = "1 w 296 396 m 316 396 l S\n306 386 m 306 406 l S\n"
    border = "1 w 0.8 0.8 0.8 rg 20 20 m 592 20 l 592 772 l 20 772 l h S\n"
    # corner marker 44x44: TL, TR, BL, BR for pages 1..4
    corners = {1: (28, 724), 2: (540, 724), 3: (28, 24), 4: (540, 24)}
    x0, y0 = corners[i]
    square = f"0 0 0 rg {x0} {y0} 44 44 re f\n"
    return f"{border}{square}{tri}{cross}{big}{top}{bot}{corner}"

objs = []
objs.append(b"<< /Type /Catalog /Pages 2 0 R >>")
objs.append(b"<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R 9 0 R] /Count 4 >>")
for i in range(1, 5):
    content = page_content(i).encode("latin-1")
    objs.append(
        f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {W} {H}] "
        f"/Resources << /Font << /F1 11 0 R /F2 12 0 R >> >> "
        f"/Contents {2 * i + 2} 0 R >>".encode("latin-1"))
    objs.append(b"<< /Length " + str(len(content)).encode() +
                b" >>\nstream\n" + content + b"\nendstream")
objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>")
objs.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")

out = bytearray(b"%PDF-1.4\n")
offsets = [0]
for i, body in enumerate(objs, 1):
    offsets.append(len(out))
    out += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
xref = len(out)
out += f"xref\n0 {len(objs) + 1}\n".encode()
out += b"0000000000 65535 f \n"
for off in offsets[1:]:
    out += f"{off:010d} 00000 n \n".encode()
out += (f"trailer\n<< /Size {len(objs) + 1} /Root 1 0 R >>\n"
        f"startxref\n{xref}\n%%EOF\n").encode()

with open(OUT, "wb") as f:
    f.write(out)
print(f"wrote {OUT} ({len(out)} bytes, {len(objs)} objects)")
