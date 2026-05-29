#!/bin/bash
# Uninstall macos-rdp-server completely.

set -euo pipefail

[[ $EUID -ne 0 ]] && { echo "Run as root: sudo bash scripts/uninstall.sh" >&2; exit 1; }

PLIST="/Library/LaunchDaemons/com.macosrdp.daemon.plist"

echo "==> Unloading launchd service..."
launchctl unload "$PLIST" 2>/dev/null || true
rm -f "$PLIST"

echo "==> Removing binary..."
rm -f /usr/local/sbin/macos-rdp-daemon

echo "==> Removing TLS certificate..."
rm -rf /etc/macos-rdp

echo "==> Removing source..."
rm -rf /usr/local/src/macos-rdp-server

echo "Done. macos-rdp-server has been removed."
echo "You may also remove Privacy & Security permissions manually in System Settings."
