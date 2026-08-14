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
- Halftoning: Auto (crisp threshold for text pages, Floyd-Steinberg
  error diffusion for photos/gradients) or forced Threshold/Diffusion
- Two-sided printing (manual duplex): odd pages print, the printer pauses
  for you to flip the paper, then even pages print flipped
- Multiple copies (printer-side)
- Printer status, toner level, page counters and estimated pages
  remaining via `p1102status` (EWS HTTP over USB, the same mechanism
  HP's own `usbink` uses)
- Supply level display in System Settings (CUPS `ReportLevels` command
  filter, like the official HP driver's `commandToHPZJS`)
- EWS proxy: browse the printer's embedded web server (page counters,
  config XML) in a browser via `./install.sh --ews`
- EconoMode (save toner), Jam Recovery and REt options

Not yet: P1102w over Wi-Fi.

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
lp -d HP_P1102 -o Duplex=DuplexTumble some-2-page.pdf
```

Alternatively add the printer in System Settings → Printers & Scanners and
pick "HP LaserJet Pro P1102, rastertozjs (open source)".

## Printer status, toner level and EWS

```
./install.sh --status          # or: build/p1102status
./install.sh --status | jq .   # --json output
./install.sh --ews             # open the printer's EWS in a browser
```

The P1102 does not answer PJL INFO queries, but it exposes a tiny embedded
web server on a vendor-specific USB interface (class FF/02/10) that speaks
plain HTTP over the bulk endpoints (the transport hplip calls the "Marvell
EWS" channel and the one HP's own `usbink` utility uses). `p1102status`
fetches `/DevMgmt/ProductStatusDyn.xml`,
`/DevMgmt/ConsumableConfigDyn.xml` and `/DevMgmt/ProductUsageDyn.xml` over
that channel and reports the printer state (e.g. `inPowerSave`), the supply
name/state/brand/serial, the toner percentage, total page counter,
cartridge page counter and the estimated pages remaining.

`ewsproxy` is the equivalent of HP's "HtmlConfig": it listens on localhost
and tunnels HTTP requests to the printer's EWS, so the printer's own pages
can be opened in a browser. The P1102 firmware only serves the `/DevMgmt/*`
XML endpoints (no HTML pages).

The PPD also declares the CUPS commands `ReportLevels` and `ReportStatus`,
so System Settings → Printers & Scanners shows the toner level for the
queue (the `commandtozjs` filter answers, same as HP's `commandToHPZJS`).
Note for driver developers: CUPS feeds the command document to the filter
as the optional argv[6] file argument (stdin is empty) and parses
`ATTR:`/`STATE:` lines from the filter's *stderr* (the job status pipe).

## About the "printer drivers are deprecated" warning

`lpadmin` may print a warning like:

```
lpadmin: printer drivers are deprecated and will stop working in a future version of CUPS
```

This is expected and harmless. CUPS 2.4+ deprecates the classic driver model
(PPD + a binary CUPS filter) in favor of *driverless IPP*, where the printer
itself describes its capabilities over IPP. This warning appears for **every**
queue created with a PPD-based driver, including the official HP, Epson and
Brother drivers — it is not specific to this project.

The queue still works normally (the warning is informational, printed only by
`lpadmin`, and macOS itself does not show it in System Settings).

For the P1102 there is no driverless alternative: IPP-USB requires IPP support
inside the printer, and the P1102 only speaks the proprietary ZjStream
language. A host-side IPP proxy would be the only "future-proof" approach,
which is a much larger project. Apple's CUPS fork still fully supports the
PPD driver model, so this driver keeps working on macOS 13-26 and beyond.

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
- Upstream foo2zjs is maintained at
  [github.com/koenkooi/foo2zjs](https://github.com/koenkooi/foo2zjs), with
  a Debian-maintained fixes branch at
  [github.com/OpenPrinting/foo2zjs](https://github.com/OpenPrinting/foo2zjs)
  (the engine we vendor is functionally identical for the P1102).
