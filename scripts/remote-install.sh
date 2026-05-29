#!/bin/bash
# One-line remote installer for macos-rdp-server.
# Usage: curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/main/scripts/remote-install.sh | sudo bash

set -euo pipefail

REPO_URL="https://github.com/grioghar/macos-rdp-server.git"
SRC_DIR="/usr/local/src/macos-rdp-server"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'

log()  { echo -e "${GREEN}==>${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
die()  { echo -e "${RED}[error]${NC} $*" >&2; exit 1; }

[[ $EUID -ne 0 ]] && die "Run as root: curl ... | sudo bash"
[[ $(uname) != "Darwin" ]] && die "macOS only."

# Minimum macOS version check (14.0 = Sonoma)
os_ver=$(sw_vers -productVersion)
major=$(echo "$os_ver" | cut -d. -f1)
[[ $major -lt 14 ]] && die "Requires macOS 14 or later (found $os_ver)"

log "macOS $os_ver — proceeding"

# ── Homebrew ──────────────────────────────────────────────────────────────
if ! command -v brew &>/dev/null; then
    log "Installing Homebrew..."
    # Run brew install as the original (non-root) user
    REAL_USER="${SUDO_USER:-$USER}"
    sudo -u "$REAL_USER" /bin/bash -c \
        "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

BREW="$(sudo -u "${SUDO_USER:-$USER}" brew --prefix)/bin/brew"

log "Installing build dependencies..."
sudo -u "${SUDO_USER:-$USER}" "$BREW" install freerdp cmake openssl 2>/dev/null || \
sudo -u "${SUDO_USER:-$USER}" "$BREW" upgrade freerdp cmake openssl 2>/dev/null || true

FREERDP_PREFIX=$(sudo -u "${SUDO_USER:-$USER}" "$BREW" --prefix freerdp3 2>/dev/null || \
                 sudo -u "${SUDO_USER:-$USER}" "$BREW" --prefix freerdp)
OPENSSL_PREFIX=$(sudo -u "${SUDO_USER:-$USER}" "$BREW" --prefix openssl)

# ── Clone / update ────────────────────────────────────────────────────────
if [[ -d "$SRC_DIR/.git" ]]; then
    log "Updating existing source in $SRC_DIR..."
    git -C "$SRC_DIR" pull --ff-only
else
    log "Cloning to $SRC_DIR..."
    git clone "$REPO_URL" "$SRC_DIR"
fi

# ── Build ─────────────────────────────────────────────────────────────────
BUILD="$SRC_DIR/build"
log "Building (this takes ~30 seconds)..."
cmake -B "$BUILD" -S "$SRC_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$FREERDP_PREFIX;$OPENSSL_PREFIX" \
    -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    --log-level=WARNING

cmake --build "$BUILD" --parallel "$(sysctl -n hw.logicalcpu)"

# ── Install binary ────────────────────────────────────────────────────────
log "Installing binary to /usr/local/sbin/macos-rdp-daemon..."
install -m 755 "$BUILD/macos-rdp-daemon" /usr/local/sbin/

# ── TLS certificate ───────────────────────────────────────────────────────
if [[ ! -f /etc/macos-rdp/server.crt ]]; then
    log "Generating self-signed TLS certificate..."
    bash "$SRC_DIR/scripts/gen-tls-cert.sh"
else
    warn "TLS certificate already exists at /etc/macos-rdp/server.crt — skipping"
fi

# ── launchd ───────────────────────────────────────────────────────────────
PLIST="/Library/LaunchDaemons/com.macosrdp.daemon.plist"
log "Installing launchd service..."
install -m 644 "$SRC_DIR/launchd/com.macosrdp.daemon.plist" "$PLIST"

# Unload first in case of reinstall
launchctl unload "$PLIST" 2>/dev/null || true
launchctl load -w "$PLIST"

# ── Privacy permissions reminder ─────────────────────────────────────────
echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  macos-rdp-server installed and running on port 3389${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "  Connect with any RDP client to: $(ipconfig getifaddr en0 2>/dev/null || echo '<your-ip>')"
echo ""
echo -e "${YELLOW}  ACTION REQUIRED:${NC} Grant two Privacy permissions in System Settings:"
echo "    1. Privacy & Security → Screen Recording → add macos-rdp-daemon"
echo "    2. Privacy & Security → Accessibility   → add macos-rdp-daemon"
echo ""
echo "  Then restart the daemon:"
echo "    sudo launchctl kickstart -k system/com.macosrdp.daemon"
echo ""
