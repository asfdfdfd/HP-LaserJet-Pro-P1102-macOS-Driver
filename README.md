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

## Features

- USB printing (macOS 13+, arm64)
- 600x600 dpi and FastRes 1200x600 dpi
- Halftoning: Auto (crisp threshold for text pages, Floyd-Steinberg error
  diffusion for photos/gradients) or forced Threshold/Diffusion
- Letter / A4 / A5 / A6 / B5 / Executive / Legal / envelopes / postcard / 16K
- Media types (envelope, letterhead, transparency, labels, ...)
- Manual feed, draft mode, print density
- Two-sided printing (manual duplex): odd pages print, the printer
  pauses, then even pages print in reverse order.  Follow HP's
  documented procedure: take the stack out of the output bin and place
  it back into the input tray **without flipping it** — printed side
  down, same orientation, top edge feeding first (the P1102 has no
  duplexer; the official HP driver works the same way).
  "Flip on Long Edge" (DuplexNoTumble, recommended) prints the backs
  rotated 180 degrees: read the back of a sheet by turning it over the
  **top** edge.  "Flip on Short Edge" (DuplexTumble) leaves the backs
  unrotated: reinsert the stack rotated 180 degrees in the plane, then
  read the back over the top edge.  (Copies are handled by CUPS and
  come out collated.)
- Printer status, toner level, page counters and estimated pages remaining
  via `p1102status` (EWS HTTP over USB, the same mechanism HP's own
  `usbink` uses)
- Supply level display in System Settings (CUPS `ReportLevels`/`ReportStatus`
  command filter, like the official HP driver's `commandToHPZJS`)
- `PrintSelfTestPage` CUPS command: prints a built-in test page
  (resolution stripes, gray blocks, alignment marks)
- EWS proxy: browse the printer's embedded web server in a browser
- EconoMode (save toner), Jam Recovery and REt options

Not yet: P1102w over Wi-Fi.

## Requirements

- macOS 13+
- Xcode Command Line Tools (`xcode-select --install`)
- [Meson](https://mesonbuild.com) + [Ninja](https://ninja-build.org):
  `brew install meson ninja`

## Build and test

```
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

The test suite verifies byte-identity of the ZjStream output against the
reference `foo2zjs -z2 -P -L0` engine (after normalizing the PJL
timestamp), plus:

- page-margin padding (the real macOS chain: cgpdftoraster renders only
  the imageable area; the filter must rebuild the full page) at 600 dpi
  and 1200x600 dpi (non-byte-aligned offset)
- manual duplex with an odd page count (blank even-page padding)
- printer-side copies (ZJI_DMCOPIES)
- self-test page content (the Bayer gray blocks must be distinct)
- halftone quality: auto-diffusion on gradients, no dithering when
  Halftone=Threshold is forced

## Install

```
tools/p1102ctl.py setup
```

This builds, installs the driver to `/Library/Printers` (asks for sudo) and
registers the printer queue `HP_P1102`. Everything in one command.

### p1102ctl subcommands

| Command | What it does |
|---|---|
| `p1102ctl setup` | build + install files + register the queue |
| `p1102ctl install` | build + install driver files only |
| `p1102ctl uninstall` | remove driver files |
| `p1102ctl add-printer` | register the CUPS queue (`lpadmin`) |
| `p1102ctl remove-printer` | remove the CUPS queue |
| `p1102ctl status` | printer status, toner, page counters (`--json`) |
| `p1102ctl ews [path]` | local HTTP proxy to the printer's EWS |
| `p1102ctl print-test-page` | print the self-test page (via libcups) |
| `p1102ctl package` | build the release `.pkg` |

Privileged subcommands re-exec themselves via sudo. The CLI is a single
Python 3 (stdlib-only) file; no bash anywhere.

Alternative to `p1102ctl install`: `sudo meson install -C build` — Meson's
install rules encode the exact `/Library/Printers` layout (binaries, bundle
Info.plist, PPD) and run the ad-hoc codesign.

## Printer status, toner level and EWS

```
p1102ctl status          # or: p1102ctl status --json
p1102ctl ews             # open the printer's EWS in a browser
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
and tunnels HTTP requests to the printer's EWS. The P1102 firmware only
serves the `/DevMgmt/*` XML endpoints (no HTML pages), so the proxy shows a
small index page with links to them.

The PPD also declares the CUPS commands `ReportLevels`, `ReportStatus` and
`PrintSelfTestPage`, so System Settings shows the toner level and a
self-test page is available. Note for driver developers: CUPS feeds the
command document to the filter as the optional argv[6] file argument
(stdin is empty) and parses `ATTR:`/`STATE:` lines from the filter's
*stderr* (the job status pipe).

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
p1102ctl package
```

Produces `HP-LaserJet-Pro-P1102-<version>.pkg` (stages via
`meson install --destdir`, builds with `pkgbuild`, registers the queue in
the postinstall script).

## Layout

```
meson.build               build system (targets, install rules, tests)
src/rastertozjs.c         CUPS filter: cups-raster -> P4 -> ZjStream
src/p1102cmd.c            libcups helper: submit CUPS commands (test page)
src/zjs/                  vendored foo2zjs engine (zjs_main) + jbigkit
status/                   p1102status, commandtozjs, ewsproxy + ews.c
bundle/Info.plist         driver bundle metadata
ppd/                      PPD for macOS
tools/p1102ctl.py         CLI (setup/install/status/ews/...)
tools/codesign.py         ad-hoc codesign at 'meson install'
tests/                    fixtures, check runners (meson test)
```

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
