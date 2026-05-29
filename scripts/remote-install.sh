#!/bin/bash
# One-line remote installer for macos-rdp-server.
# Usage: curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/main/scripts/remote-install.sh | sudo bash
#
# Downloads the latest pre-built universal binary from GitHub Releases.
# Falls back to building from source if no binary is available.

set -euo pipefail

REPO="grioghar/macos-rdp-server"
PLIST_SRC="https://raw.githubusercontent.com/$REPO/main/launchd/com.macosrdp.daemon.plist"
CERT_SCRIPT="https://raw.githubusercontent.com/$REPO/main/scripts/gen-tls-cert.sh"
PLIST_DEST="/Library/LaunchDaemons/com.macosrdp.daemon.plist"
BINARY_DEST="/usr/local/sbin/macos-rdp-daemon"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}==>${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
die()  { echo -e "${RED}[error]${NC} $*" >&2; exit 1; }

[[ $EUID -ne 0 ]] && die "Run as root: curl ... | sudo bash"
[[ $(uname) != "Darwin" ]] && die "macOS only."

os_ver=$(sw_vers -productVersion)
major=$(echo "$os_ver" | cut -d. -f1)
[[ $major -lt 14 ]] && die "Requires macOS 14 or later (found $os_ver)"

ARCH=$(uname -m)   # arm64 or x86_64
log "macOS $os_ver on $ARCH — proceeding"

# ── Try to download a pre-built binary ───────────────────────────────────
LATEST_TAG=$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" \
             | grep '"tag_name"' | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/')

if [[ -n "$LATEST_TAG" ]]; then
    log "Latest release: $LATEST_TAG — downloading pre-built universal binary..."
    BINARY_URL="https://github.com/$REPO/releases/download/$LATEST_TAG/macos-rdp-daemon-universal"

    TMP_BIN=$(mktemp)
    if curl -fsSL "$BINARY_URL" -o "$TMP_BIN" 2>/dev/null; then
        install -m 755 "$TMP_BIN" "$BINARY_DEST"
        rm -f "$TMP_BIN"
        log "Binary installed from release $LATEST_TAG"
        BUILT_FROM_SOURCE=false
    else
        warn "Pre-built binary unavailable — falling back to build from source"
        rm -f "$TMP_BIN"
        BUILT_FROM_SOURCE=true
    fi
else
    warn "Could not determine latest release — building from source"
    BUILT_FROM_SOURCE=true
fi

# ── Build from source (fallback) ──────────────────────────────────────────
if [[ "${BUILT_FROM_SOURCE:-true}" == "true" ]]; then
    REAL_USER="${SUDO_USER:-$USER}"
    SRC_DIR="/usr/local/src/macos-rdp-server"

    if ! command -v brew &>/dev/null; then
        log "Installing Homebrew..."
        sudo -u "$REAL_USER" /bin/bash -c \
            "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    fi

    BREW="$(sudo -u "$REAL_USER" brew --prefix)/bin/brew"
    log "Installing build dependencies..."
    sudo -u "$REAL_USER" "$BREW" install freerdp cmake openssl 2>/dev/null || true

    FREERDP_PREFIX=$(sudo -u "$REAL_USER" "$BREW" --prefix freerdp3 2>/dev/null || \
                     sudo -u "$REAL_USER" "$BREW" --prefix freerdp)
    OPENSSL_PREFIX=$(sudo -u "$REAL_USER" "$BREW" --prefix openssl)

    if [[ -d "$SRC_DIR/.git" ]]; then
        log "Updating source in $SRC_DIR..."
        git -C "$SRC_DIR" pull --ff-only
    else
        log "Cloning source to $SRC_DIR..."
        git clone "https://github.com/$REPO.git" "$SRC_DIR"
    fi

    BUILD="$SRC_DIR/build"
    log "Building..."
    cmake -B "$BUILD" -S "$SRC_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$FREERDP_PREFIX;$OPENSSL_PREFIX" \
        -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
        --log-level=WARNING
    cmake --build "$BUILD" --parallel "$(sysctl -n hw.logicalcpu)"
    install -m 755 "$BUILD/macos-rdp-daemon" "$BINARY_DEST"
fi

# ── TLS certificate ───────────────────────────────────────────────────────
if [[ ! -f /etc/macos-rdp/server.crt ]]; then
    log "Generating self-signed TLS certificate..."
    bash <(curl -fsSL "$CERT_SCRIPT")
else
    warn "TLS certificate already exists at /etc/macos-rdp/server.crt — skipping"
fi

# ── launchd ───────────────────────────────────────────────────────────────
log "Installing launchd service..."
curl -fsSL "$PLIST_SRC" -o "$PLIST_DEST"
chmod 644 "$PLIST_DEST"
launchctl unload "$PLIST_DEST" 2>/dev/null || true
launchctl load -w "$PLIST_DEST"

# ── Done ──────────────────────────────────────────────────────────────────
IP=$(ipconfig getifaddr en0 2>/dev/null || \
     ipconfig getifaddr en1 2>/dev/null || echo '<your-ip>')

echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  macos-rdp-server installed and running on port 3389${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo ""
echo "  Connect to: $IP"
echo ""
echo -e "${YELLOW}  ACTION REQUIRED — grant two Privacy permissions:${NC}"
echo "    System Settings → Privacy & Security → Screen Recording → add macos-rdp-daemon"
echo "    System Settings → Privacy & Security → Accessibility   → add macos-rdp-daemon"
echo ""
echo "  Then restart the daemon:"
echo "    sudo launchctl kickstart -k system/com.macosrdp.daemon"
echo ""
