#!/usr/bin/env python3
"""Semantic manual-duplex check.

The byte-identity tests only prove the filter matches the vendored
engine; they cannot catch a wrong page order, a missing rotation or a
misplaced pause.  This check decodes the actual pages of the ZjStream
and verifies the ZJS engine's manual-duplex behavior (verified on the
P1606dn duplexer):

  odd pages in order -> ZJT_2600N_PAUSE -> even pages in REVERSE order
  (Duplex=DuplexNoTumble, long edge: each back rotated 180 degrees;
   Duplex=DuplexTumble, short edge: backs unrotated)
  and each page printed once (copies are handled by CUPS).

Usage:
  check_duplex.py --filter BIN --mkraster BIN --zjsdump BIN
                  --opts "Duplex=DuplexNoTumble" --copies 2 --rotate
                  [--no-rotate]
"""
import argparse
import os
import subprocess
import sys
import tempfile

W, H = 1200, 1600          # fixture page size (pixels)
PAGES = 4
SQUARE = 200               # marker square size
Y0 = [100 + (i - 1) * 350 for i in range(PAGES + 1)]  # per-page top y


def gen_pages(td):
    """4 pages, each with one 200x200 black square at x=100..300,
    y = Y0[i].  After a 180-degree rotation the square lands in the
    right column (x = 980..1179 on the 1280-wide decoded page) with a
    mirrored y, so the decoded bbox identifies page and orientation."""
    pages = []
    for i in range(1, PAGES + 1):
        y0 = Y0[i]
        pbm = bytearray(b"P4\n%d %d\n" % (W, H))
        bpl = (W + 7) // 8
        for y in range(H):
            row = bytearray(bpl)
            if y0 <= y < y0 + SQUARE:
                for x in range(100, 300):
                    row[x // 8] |= 0x80 >> (x % 8)
            pbm += row
        pages.append(bytes(pbm))
    with open(f"{td}/pages.pbm", "wb") as f:
        for p in pages:
            f.write(p)


def identify(x0, y1):
    """(page, rotated) from the decoded bbox of a fixture page.

    mkraster sets a Letter page size, so the filter pads the 1200x1600
    fixture to 5100x6600 (top-left aligned).  The engine pads the width
    to a multiple of 128 (5120).  Unrotated: square x=100..300.  After a
    180-degree rotation the square lands at x=4820..5020 with a mirrored
    y (y1 = 6600 - Y0[i])."""
    if x0 < 2500:                                   # left: unrotated
        return ((y1 - SQUARE + 1 - 100) // 350 + 1, False)
    return ((6600 - y1 - 100) // 350 + 1, True)


def run(args):
    expected = ["P1", "P3", "PAUSE", "P4", "P2"]
    if args.rotate:
        expected = ["P1", "P3", "PAUSE", "P4r", "P2r"]

    with tempfile.TemporaryDirectory(prefix="p1102dup-") as td:
        gen_pages(td)
        with open(f"{td}/pages.pbm", "rb") as fin, \
                open(f"{td}/pages.raster", "wb") as fout:
            subprocess.run([args.mkraster], stdin=fin, stdout=fout,
                           check=True)
        with open(f"{td}/pages.raster", "rb") as fin, \
                open(f"{td}/out.zjs", "wb") as fout:
            subprocess.run([args.filter, "1", "user", "Duplex",
                            str(args.copies), args.opts],
                           stdin=fin, stdout=fout, check=True)

        r = subprocess.run([os.path.abspath(args.zjsdump),
                            f"{td}/out.zjs"],
                           capture_output=True, text=True, cwd=td)
        if r.returncode != 0:
            print(f"FAIL: zjsdump failed ({r.stderr.strip()})",
                  file=sys.stderr)
            return 1

        seq, copies = [], []
        pending = None
        for line in r.stdout.splitlines():
            line = line.strip()
            if line.startswith("PAGE "):
                m = __import__("re").search(r"copies=(\d+)", line)
                copies.append(int(m.group(1)) if m else 0)
            elif line.startswith("bbox x="):
                m = __import__("re").search(
                    r"bbox x=(\d+)\.\.\d+ y=\d+\.\.(\d+)", line)
                page, rotated = identify(int(m.group(1)), int(m.group(2)))
                seq.append(f"P{page}" + ("r" if rotated else ""))
            elif line == "PAUSE":
                seq.append("PAUSE")

        if seq != expected:
            print(f"FAIL: page sequence {seq} != expected {expected}",
                  file=sys.stderr)
            return 1
        # CUPS handles copies (PPD: *cupsManualCopies: False): the
        # raster arrives pre-expanded, so the engine must print each
        # page once even when argv says copies=2.
        if copies != [1] * 4:
            print(f"FAIL: copies {copies} != [1, 1, 1, 1] "
                  f"(argv copies={args.copies} must be ignored)",
                  file=sys.stderr)
            return 1

    print(f"PASS duplex-order {'longedge' if args.rotate else 'shortedge'}"
          f" (sequence {seq})")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", required=True)
    ap.add_argument("--mkraster", required=True)
    ap.add_argument("--zjsdump", required=True)
    ap.add_argument("--opts", required=True)
    ap.add_argument("--copies", type=int, default=1)
    ap.add_argument("--rotate", action="store_true",
                    help="backs must be rotated 180 degrees")
    sys.exit(run(ap.parse_args()))


if __name__ == "__main__":
    main()
