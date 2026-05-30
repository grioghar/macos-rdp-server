#!/bin/bash
# macos-rdp-server one-line installer.
# Usage: curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/master/scripts/remote-install.sh | sudo bash

set -euo pipefail

REPO="grioghar/macos-rdp-server"
BINARY_DEST="/usr/local/sbin/macos-rdp-daemon"
PLIST_DEST="/Library/LaunchDaemons/com.macosrdp.daemon.plist"
RAW="https://raw.githubusercontent.com/$REPO/master"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}==>${NC} $*"; }
warn() { echo -e "${YELLOW}warn:${NC} $*"; }
die()  { echo -e "${RED}error:${NC} $*" >&2; exit 1; }

[[ $EUID -ne 0 ]]   && die "Run as root: curl ... | sudo bash"
[[ $(uname) != Darwin ]] && die "macOS only."
major=$(sw_vers -productVersion | cut -d. -f1)
[[ $major -lt 14 ]] && die "Requires macOS 14 or later (found $(sw_vers -productVersion))"

log "macOS $(sw_vers -productVersion) · $(uname -m)"

# /usr/local/sbin does not exist on a clean macOS — create it (and any other
# destination dirs) up front so `install` can't fail on a missing parent.
mkdir -p "$(dirname "$BINARY_DEST")" "$(dirname "$PLIST_DEST")"

# ── Download pre-built universal binary from latest release ──────────────
# The `|| true` keeps a rate-limited / offline API response from aborting the
# script under `set -e` — an empty LATEST falls through to the source build.
LATEST=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" 2>/dev/null \
         | grep '"tag_name"' | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/' || true)

if [[ -n "$LATEST" ]]; then
    BIN_URL="https://github.com/$REPO/releases/download/$LATEST/macos-rdp-daemon"
    log "Downloading macos-rdp-daemon $LATEST (universal)..."
    TMP=$(mktemp)
    if curl -fsSL "$BIN_URL" -o "$TMP"; then
        install -m 755 "$TMP" "$BINARY_DEST"
        rm -f "$TMP"
        log "Binary installed ($LATEST)"
        FROM_RELEASE=true
    else
        warn "Release download failed — building from source"
        rm -f "$TMP"
        FROM_RELEASE=false
    fi
else
    warn "Could not resolve latest release — building from source"
    FROM_RELEASE=false
fi

# ── Fallback: build from source ───────────────────────────────────────────
if [[ "${FROM_RELEASE:-false}" != "true" ]]; then
    REAL_USER="${SUDO_USER:-$USER}"
    SRC="/usr/local/src/macos-rdp-server"

    if ! command -v brew &>/dev/null; then
        log "Installing Homebrew..."
        sudo -u "$REAL_USER" /bin/bash -c \
            "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi

    BREW="$(sudo -u "$REAL_USER" brew --prefix)/bin/brew"
    sudo -u "$REAL_USER" "$BREW" install freerdp cmake openssl 2>/dev/null || true

    FP=$(sudo -u "$REAL_USER" "$BREW" --prefix freerdp3 2>/dev/null || \
         sudo -u "$REAL_USER" "$BREW" --prefix freerdp)
    OP=$(sudo -u "$REAL_USER" "$BREW" --prefix openssl)

    [[ -d "$SRC/.git" ]] \
        && git -C "$SRC" pull --ff-only \
        || git clone "https://github.com/$REPO.git" "$SRC"

    cmake -B "$SRC/build" -S "$SRC" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$FP;$OP" \
        -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
        --log-level=WARNING
    cmake --build "$SRC/build" --parallel "$(sysctl -n hw.logicalcpu)"
    install -m 755 "$SRC/build/macos-rdp-daemon" "$BINARY_DEST"
fi

# ── TLS certificate ───────────────────────────────────────────────────────
if [[ ! -f /etc/macos-rdp/server.crt ]]; then
    log "Generating TLS certificate..."
    bash <(curl -fsSL "$RAW/scripts/gen-tls-cert.sh")
else
    warn "TLS cert exists at /etc/macos-rdp/server.crt — skipping"
fi

# ── launchd ───────────────────────────────────────────────────────────────
log "Installing launchd service..."
curl -fsSL "$RAW/launchd/com.macosrdp.daemon.plist" -o "$PLIST_DEST"
chmod 644 "$PLIST_DEST"
chown root:wheel "$PLIST_DEST"

# Prefer the modern bootstrap/bootout API (macOS 11+); fall back to legacy
# load/unload on older systems. Neither failure should abort the install.
launchctl bootout system "$PLIST_DEST" 2>/dev/null || \
    launchctl unload "$PLIST_DEST" 2>/dev/null || true
if launchctl bootstrap system "$PLIST_DEST" 2>/dev/null; then
    launchctl enable system/com.macosrdp.daemon 2>/dev/null || true
elif ! launchctl load -w "$PLIST_DEST" 2>/dev/null; then
    warn "could not register the launchd service automatically"
    warn "load it manually: sudo launchctl bootstrap system $PLIST_DEST"
fi

# ── Done ──────────────────────────────────────────────────────────────────
IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || echo '<your-ip>')

echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  macos-rdp-server installed · listening on port 3389${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "  Connect to: $IP"
echo ""
echo -e "${YELLOW}  ACTION REQUIRED — grant two Privacy permissions:${NC}"
echo "    System Settings → Privacy & Security → Screen Recording → add macos-rdp-daemon"
echo "    System Settings → Privacy & Security → Accessibility   → add macos-rdp-daemon"
echo ""
echo "  Then:  sudo launchctl kickstart -k system/com.macosrdp.daemon"
echo ""
