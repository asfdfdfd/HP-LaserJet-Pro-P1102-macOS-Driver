#!/bin/bash
# install.sh - build and install the open-source HP LaserJet P1102 driver.
# Run with sudo:  sudo ./install.sh
#
# Installs:
#   /Library/Printers/HP/p1102raster.bundle/Contents/MacOS/rastertozjs
#   /Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1102.ppd
# and registers the printer queue "HP_P1102".

set -e

BUNDLE_DIR="/Library/Printers/HP/p1102raster.bundle"
BIN_DIR="$BUNDLE_DIR/Contents/MacOS"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"
PPD_SRC="ppd/HP-LaserJet-Pro-P1102.ppd"
PPD_DST="$PPD_DIR/HP LaserJet Professional P1102.ppd"
QUEUE="HP_P1102"

if [ "$(id -u)" != "0" ]; then
    echo "Run as root: sudo $0"
    exit 1
fi

echo "==> Building filter..."
make clean >/dev/null 2>&1 || true
make

echo "==> Installing filter bundle..."
mkdir -p "$BIN_DIR"
cp build/rastertozjs "$BIN_DIR/rastertozjs"
mkdir -p "$BUNDLE_DIR/Contents"
cat > "$BUNDLE_DIR/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>English</string>
	<key>CFBundleExecutable</key>
	<string>rastertozjs</string>
	<key>CFBundleIdentifier</key>
	<string>org.opencode.hp-p1102-raster</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>P1102Raster</string>
	<key>CFBundlePackageType</key>
	<string>BNDL</string>
	<key>CFBundleShortVersionString</key>
	<string>1.0.0</string>
	<key>CFBundleSignature</key>
	<string>????</string>
	<key>CFBundleVersion</key>
	<string>1.0.0</string>
</dict>
</plist>
EOF
chown -R root:wheel "$BUNDLE_DIR"
chmod 755 "$BUNDLE_DIR" "$BUNDLE_DIR/Contents" "$BIN_DIR"
chmod 555 "$BIN_DIR/rastertozjs"
codesign --force -s - "$BIN_DIR/rastertozjs" || true

echo "==> Installing PPD..."
mkdir -p "$PPD_DIR"
cp "$PPD_SRC" "$PPD_DST"
chown root:wheel "$PPD_DST"
chmod 644 "$PPD_DST"

echo "==> Registering printer queue..."
# Find the printer's USB URI
URI=$(lpinfo -v 2>/dev/null | grep -i "usb://" | grep -i "p1102" | head -1 | awk '{print $2}')
if [ -z "$URI" ]; then
    echo "WARNING: P1102 USB device not found via lpinfo; create the queue"
    echo "         manually in System Settings -> Printers & Scanners."
    echo "         Driver to pick: HP LaserJet Pro P1102, rastertozjs."
else
    echo "Using device: $URI"
    lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD_DST" -o printer-is-shared=false \
        -D "HP LaserJet Professional P1102 (open source driver)"
fi

echo "==> Done."
echo "Print a test page:  lp -d $QUEUE some.pdf"
