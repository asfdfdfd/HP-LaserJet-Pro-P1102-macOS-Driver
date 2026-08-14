#!/bin/bash
# build_pkg.sh - build the release .pkg installer for the P1102 driver.
#   make && ./package/build_pkg.sh
set -e
cd "$(dirname "$0")/.."

if [ "$(id -u)" != "0" ]; then
    echo "==> Re-running with sudo (needed for root:wheel ownership)..."
    exec sudo "$0" "$@"
fi

VERSION="1.0.0"
PKG="HP-LaserJet-Pro-P1102-${VERSION}.pkg"
IDENTIFIER="org.opencode.hp-p1102-driver"

ROOT=$(mktemp -d)
SCRIPTS=$(mktemp -d)
trap 'rm -rf "$ROOT" "$SCRIPTS"' EXIT

BIN_DIR="$ROOT/Library/Printers/HP/p1102raster.bundle/Contents/MacOS"
PPD_DIR="$ROOT/Library/Printers/PPDs/Contents/Resources"

echo "==> staging payload..."
mkdir -p "$BIN_DIR" "$PPD_DIR"
cp build/rastertozjs "$BIN_DIR/"
cp build/p1102status build/commandtozjs build/ewsproxy "$BIN_DIR/"
cp ppd/HP-LaserJet-Pro-P1102.ppd "$PPD_DIR/HP LaserJet Professional P1102.ppd"

cat > "$ROOT/Library/Printers/HP/p1102raster.bundle/Contents/Info.plist" <<'EOF'
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

echo "==> setting ownership (root:wheel)..."
chown -R root:wheel "$ROOT"
chmod 755 "$ROOT" "$ROOT/Library" "$ROOT/Library/Printers" \
    "$ROOT/Library/Printers/HP" \
    "$ROOT/Library/Printers/HP/p1102raster.bundle" \
    "$ROOT/Library/Printers/HP/p1102raster.bundle/Contents" \
    "$ROOT/Library/Printers/HP/p1102raster.bundle/Contents/MacOS" \
    "$ROOT/Library/Printers/PPDs" "$ROOT/Library/Printers/PPDs/Contents" \
    "$ROOT/Library/Printers/PPDs/Contents/Resources"
chmod 555 "$BIN_DIR/rastertozjs" "$BIN_DIR/p1102status"
chmod 644 "$BIN_DIR/../Info.plist" \
    "$PPD_DIR/HP LaserJet Professional P1102.ppd"

echo "==> signing (ad-hoc)..."
for t in rastertozjs p1102status commandtozjs ewsproxy; do
    codesign --force -s - "$BIN_DIR/$t"
done

echo "==> writing postinstall script..."
cat > "$SCRIPTS/postinstall" <<'EOF'
#!/bin/bash
# Register the printer queue after the driver files are in place.
QUEUE="HP_P1102"
PPD="/Library/Printers/PPDs/Contents/Resources/HP LaserJet Professional P1102.ppd"
URI=$(lpinfo -v 2>/dev/null | grep -i "usb://" | grep -i "p1102" | head -1 | awk '{print $2}')
if [ -n "$URI" ]; then
    lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD" -o printer-is-shared=false \
        -D "HP LaserJet Professional P1102 (open source driver)"
fi
exit 0
EOF
chmod 755 "$SCRIPTS/postinstall"

echo "==> building package..."
pkgbuild \
    --root "$ROOT" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --ownership recommended \
    --scripts "$SCRIPTS" \
    "$PKG"

echo "==> $PKG"
echo "Install:  sudo installer -pkg $PKG -target /"
echo "Remove:   sudo pkgutil --forget $IDENTIFIER"
