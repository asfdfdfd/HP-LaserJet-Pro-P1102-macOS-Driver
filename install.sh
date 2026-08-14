#!/bin/bash
# install.sh - build and install the open-source HP LaserJet P1102 driver.
#
#   ./install.sh            build + install (asks for sudo)
#   ./install.sh --uninstall
#   ./install.sh --print-test
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
TESTPDF="tests/testpage.pdf"

cd "$(dirname "$0")"

need_root() {
    if [ "$(id -u)" != "0" ]; then
        echo "==> Re-running with sudo for the privileged parts..."
        exec sudo "$0" "$@"
    fi
}

find_usb_uri() {
    lpinfo -v 2>/dev/null | grep -i "usb://" | grep -i "p1102" | head -1 | awk '{print $2}'
}

do_build() {
    echo "==> Building filter..."
    make clean >/dev/null 2>&1 || true
    make
    [ -x build/rastertozjs ] || { echo "Build failed"; exit 1; }
}

do_install() {
    echo "==> Installing filter bundle..."
    mkdir -p "$BIN_DIR"
    cp build/rastertozjs "$BIN_DIR/rastertozjs"
    mkdir -p "$BUNDLE_DIR/Contents"
    cat > "$BUNDLE_DIR/Contents/Info.plist" <<'EOF'
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
    codesign --force -s - "$BIN_DIR/rastertozjs" >/dev/null 2>&1 || true

    echo "==> Installing status tool..."
    cp build/p1102status "$BIN_DIR/p1102status"
    chmod 555 "$BIN_DIR/p1102status"
    codesign --force -s - "$BIN_DIR/p1102status" >/dev/null 2>&1 || true

    echo "==> Installing PPD..."
    mkdir -p "$PPD_DIR"
    cp "$PPD_SRC" "$PPD_DST"
    chown root:wheel "$PPD_DST"
    chmod 644 "$PPD_DST"

    echo "==> Registering printer queue..."
    URI=$(find_usb_uri)
    if [ -z "$URI" ]; then
        echo "WARNING: P1102 USB device not found via lpinfo."
        echo "         Add the printer manually in System Settings ->"
        echo "         Printers & Scanners (driver: HP LaserJet Pro P1102,"
        echo "         rastertozjs (open source))."
    else
        echo "Using device: $URI"
        lpadmin -p "$QUEUE" -E -v "$URI" -P "$PPD_DST" -o printer-is-shared=false \
            -D "HP LaserJet Professional P1102 (open source driver)"
    fi
}

do_uninstall() {
    lpadmin -x "$QUEUE" >/dev/null 2>&1 || true
    rm -rf "$BUNDLE_DIR"
    rm -f "$PPD_DST"
    echo "==> Removed queue '$QUEUE', driver bundle and PPD."
}

do_print_test() {
    if ! lpstat -p "$QUEUE" >/dev/null 2>&1; then
        echo "Queue '$QUEUE' does not exist. Run: sudo $0"
        exit 1
    fi
    [ -f "$TESTPDF" ] || python3 - "$TESTPDF" <<'PYEOF'
import sys
def build(out):
    objects = []
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
    objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                   b"/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>")
    objects.append(b"<< /Length 200 >>\nstream\n"
                   b"BT /F1 36 Tf 72 720 Td (HP P1102 open source driver) Tj ET\n"
                   b"0 0 1 rg 72 600 468 100 re f\n"
                   b"0 0 0 RG 2 w 72 500 468 60 re S\n"
                   b"endstream")
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    out.write(b"%PDF-1.4\n")
    offsets = []
    for i, obj in enumerate(objects, 1):
        offsets.append(out.tell())
        out.write(b"%d 0 obj\n" % i)
        out.write(obj)
        out.write(b"\nendobj\n")
    xref = out.tell()
    out.write(b"xref\n0 %d\n" % (len(objects) + 1))
    out.write(b"0000000000 65535 f \n")
    for off in offsets:
        out.write(b"%010d 00000 n \n" % off)
    out.write(b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n"
              % (len(objects) + 1, xref))
with open(sys.argv[1], "wb") as f:
    build(f)
PYEOF
    echo "==> Printing test page to '$QUEUE'..."
    lp -d "$QUEUE" "$TESTPDF"
    echo "    Check /var/log/cups/error_log on failure."
}

case "${1:-install}" in
    install)
        do_build
        if [ "$(id -u)" != "0" ]; then
            echo "==> Re-running with sudo for the privileged parts..."
            exec sudo "$0" __install
        fi
        do_install
        echo "==> Done. Print a test page: ./install.sh --print-test"
        ;;
    __install)
        do_install
        echo "==> Done. Print a test page: ./install.sh --print-test"
        ;;
    --print-test)
        do_print_test
        ;;
    --status)
        if [ "$(id -u)" = "0" ]; then
            echo "Run without sudo: $0 --status"
            exit 1
        fi
        "$BIN_DIR/p1102status"
        ;;
    --uninstall)
        need_root "$@"
        do_uninstall
        ;;
    *)
        echo "Usage: $0 [install|--print-test|--status|--uninstall]"
        exit 1
        ;;
esac
