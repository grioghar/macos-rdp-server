#!/bin/bash
# build.sh — build the macOS RDP menu-bar config app and wrap it in a proper
# .app bundle so LSUIElement (no Dock icon) takes effect.
#
# Usage:
#   gui/build.sh                # release build -> gui/build/MacOSRDPConfig.app
#   gui/build.sh --compile-only # just compile (used by CI); no bundle
#
# Requires the Swift toolchain (ships with Xcode / Command Line Tools).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

CONFIG=release
COMPILE_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --compile-only) COMPILE_ONLY=1 ;;
        --debug)        CONFIG=debug ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

echo "==> swift build ($CONFIG)"
swift build -c "$CONFIG"

if [[ "$COMPILE_ONLY" -eq 1 ]]; then
    echo "Compile-only: skipping .app bundling."
    exit 0
fi

BIN="$(swift build -c "$CONFIG" --show-bin-path)/MacOSRDPConfig"
[[ -x "$BIN" ]] || { echo "build did not produce $BIN" >&2; exit 1; }

APP="$HERE/build/MacOSRDPConfig.app"
echo "==> bundling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/MacOSRDPConfig"
cp "$HERE/Info.plist" "$APP/Contents/Info.plist"

# Ad-hoc sign so it launches without Gatekeeper griping on the dev box.
codesign --force --sign - "$APP" >/dev/null 2>&1 || \
    echo "(codesign skipped/failed — app still runs locally)"

cat <<EOF

Built: $APP

Run it:
  open "$APP"

It appears as a menu-bar icon (no Dock icon). Use the menu to start/stop the
RDP daemon and toggle its options.
EOF
