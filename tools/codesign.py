#!/usr/bin/env python3
"""Ad-hoc codesign the installed driver binaries (runs at 'meson install')."""
import os
import subprocess
import sys

BIN_DIR = "/Library/Printers/HP/p1102raster.bundle/Contents/MacOS"
BINARIES = ("rastertozjs", "p1102status", "commandtozjs", "ewsproxy", "p1102cmd")

destdir = os.environ.get("MESON_INSTALL_DESTDIR_PREFIX", "")
base = os.path.join(destdir, BIN_DIR.lstrip("/")) if destdir else BIN_DIR

rc = 0
for name in BINARIES:
    path = os.path.join(base, name)
    if not os.path.exists(path):
        continue
    r = subprocess.run(["codesign", "--force", "-s", "-", path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        print(f"codesign: warning: {name}: rc={r.returncode}", file=sys.stderr)
        rc = 1
sys.exit(rc)
