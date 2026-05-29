#!/bin/bash
# Build and install macos-rdp-daemon.
# Must run as root. Requires Homebrew freerdp3 and Xcode Command Line Tools.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build"

echo "==> Building daemon..."
cmake -B "$BUILD" -S "$REPO" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(brew --prefix freerdp3):$(brew --prefix openssl)"
cmake --build "$BUILD" --parallel "$(sysctl -n hw.logicalcpu)"

echo "==> Installing binary..."
install -m 755 "$BUILD/macos-rdp-daemon" /usr/local/sbin/

echo "==> Generating TLS certificate..."
"$REPO/scripts/gen-tls-cert.sh"

echo "==> Installing launchd plist..."
install -m 644 "$REPO/launchd/com.macosrdp.daemon.plist" \
    /Library/LaunchDaemons/

echo "==> Loading daemon..."
launchctl load -w /Library/LaunchDaemons/com.macosrdp.daemon.plist

echo ""
echo "Done. RDP server is listening on port 3389."
echo "Connect with any RDP client (Windows Remote Desktop, FreeRDP, Remmina)."
echo ""
echo "To install the HID DriverKit extension, open the Xcode project in"
echo "hid-driver/ and sign it with your Apple Developer account, then:"
echo "  systemextensionsctl activate com.macosrdp.hid-driver"
