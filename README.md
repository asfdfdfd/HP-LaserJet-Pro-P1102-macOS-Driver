# HP LaserJet Professional P1102 — open source driver for macOS

The official HP driver stopped installing on modern macOS. This project is a
fully open source (GPL-2.0) replacement: a CUPS filter that converts the
CUPS raster produced by macOS into the proprietary **ZjStream (ZJS)** format
the P1102 understands, plus a PPD.

No kernel extensions, no proprietary HP binaries, no SIP changes.

## How it works

```
Your PDF  →  cgpdftoraster (built into macOS)  →  CUPS raster (stdin)
         →  rastertozjs (this project)         →  ZJS + PJL stream (stdout)
         →  usb backend (built into macOS)     →  printer
```

The ZjStream encoder is vendored from
[foo2zjs](https://github.com/koenkooi/foo2zjs) (GPL-2.0-or-later, model
`-z2` for the P1102/P1566/P1606 family). The output of this filter is
byte-identical to `foo2zjs -z2 -P -L0` for the same bitmap, which is
verified by the test suite.

## Supported

- USB printing (macOS 13+, arm64)
- 600x600 dpi and FastRes 1200x600 dpi
- Letter / A4 / A5 / A6 / B5 / Executive / Legal / envelopes / postcard / 16K
- Media types (envelope, letterhead, transparency, labels, ...)
- Manual feed, draft mode, print density
- Multiple copies (printer-side)

Not yet: toner status display (see `status/`), P1102w over Wi-Fi.

## Install (developer / single machine)

```
make
sudo ./install.sh
```

This installs:

- `/Library/Printers/HP/p1102raster.bundle/Contents/MacOS/rastertozjs`
- `/Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1102.ppd`
- a printer queue `HP_P1102` on the USB device

Then print:

```
lp -d HP_P1102 tests/testpage.pdf
```

Alternatively add the printer in System Settings → Printers & Scanners and
pick "HP LaserJet Pro P1102, rastertozjs (open source)".

## Building a release .pkg

```
make
package/build_pkg.sh
```

## Layout

```
src/rastertozjs.c        CUPS filter: cups-raster -> P4 -> ZjStream
src/zjs/                 vendored foo2zjs engine (zjs_main) + jbigkit
ppd/                     PPD for macOS
tests/                   unit tests + test helpers
status/                  toner status tool (WIP)
package/                 pkgbuild scripts
```

## Testing

```
make test
```

The test suite generates CUPS raster input from known PBM fixtures, runs the
filter, and compares the ZjStream output byte-for-byte (after normalizing the
PJL timestamp) against the reference `foo2zjs -z2 -P -L0` binary.

## Credits

- Rick Richardson and contributors — foo2zjs engine, GPL-2.0-or-later
- Markus Kuhn — jbigkit (bundled with foo2zjs)
- The Zenographics ZjStream protocol was reverse engineered by the foo2zjs
  community; it is also used by HP's own drivers (`rastertozjs` in the
  official HP package).
