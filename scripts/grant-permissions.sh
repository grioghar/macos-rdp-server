#!/bin/bash
# Pre-grant TCC permissions for macos-rdp-daemon.
# Requires SIP to be disabled or partially disabled.
# Run ONCE after install if you want to skip the System Settings prompts.

set -euo pipefail

[[ $EUID -ne 0 ]] && { echo "Run as root." >&2; exit 1; }

BINARY="/usr/local/sbin/macos-rdp-daemon"
[[ ! -f "$BINARY" ]] && { echo "Install first: sudo bash scripts/install.sh" >&2; exit 1; }

# Screen Recording (kTCCServiceScreenCapture)
sqlite3 /Library/Application\ Support/com.apple.TCC/TCC.db \
  "INSERT OR REPLACE INTO access VALUES(
     'kTCCServiceScreenCapture','$BINARY',1,2,4,1,NULL,NULL,0,'UNUSED',NULL,0,
     strftime('%s','now'));" 2>/dev/null && \
  echo "Granted: Screen Recording" || \
  echo "Could not grant Screen Recording — disable SIP first (csrutil disable from Recovery)"

# Accessibility (kTCCServiceAccessibility)
sqlite3 /Library/Application\ Support/com.apple.TCC/TCC.db \
  "INSERT OR REPLACE INTO access VALUES(
     'kTCCServiceAccessibility','$BINARY',1,2,4,1,NULL,NULL,0,'UNUSED',NULL,0,
     strftime('%s','now'));" 2>/dev/null && \
  echo "Granted: Accessibility" || \
  echo "Could not grant Accessibility — disable SIP first"

echo ""
echo "Restarting daemon..."
launchctl kickstart -k system/com.macosrdp.daemon 2>/dev/null || \
  launchctl stop com.macosrdp.daemon 2>/dev/null || true
