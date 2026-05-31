#!/bin/bash
# install-user.sh — durable, sudo-free install of macos-rdp-daemon as a per-user
# LaunchAgent in the Aqua (GUI) session.
#
# Why this exists instead of install.sh's LaunchDaemon:
#   * Screen capture requires the daemon to run inside the user's GUI (Aqua) session
#     so it has a WindowServer connection. A system LaunchDaemon does NOT, which is why
#     the daemon rendered a black screen under the old install.
#   * macOS TCC pins the Screen-Recording / Accessibility grant to the binary's code
#     signature. An ad-hoc / linker signature gets a new cdhash on every rebuild, so the
#     grant breaks each time you update. This script signs the binary with a STABLE
#     self-signed certificate, so TCC pins to the (identifier + cert) requirement and the
#     grant SURVIVES rebuilds. Re-running this script to deploy a new build keeps the grant.
#   * Everything lives under $HOME, so no sudo is ever required.
#
# Usage:
#   scripts/install-user.sh [path-to-macos-rdp-daemon]
#     If no path is given, falls back to ./build/macos-rdp-daemon.
#
# NOTE: This codifies the exact sequence validated manually over SSH. Verify end-to-end
# on a real Mac after any change.

set -euo pipefail

if [[ $EUID -eq 0 ]]; then
    echo "Run as your normal user, NOT root — this installs a per-user LaunchAgent." >&2
    exit 1
fi

PORT="${RDP_PORT:-3389}"
ROOT="$HOME/.macos-rdp"
BIN_DIR="$ROOT/bin"
SIGN_DIR="$ROOT/signing"
BIN="$BIN_DIR/macos-rdp-daemon"
PLIST="$HOME/Library/LaunchAgents/com.macosrdp.agent.plist"
LABEL="com.macosrdp.agent"
GUI="gui/$(id -u)"
KC="$SIGN_DIR/macos-rdp.keychain-db"
KCPASS="macosrdp"
P12PASS="temp12"
CN="macos-rdp-signing"

# 1. Locate the binary to install ------------------------------------------------------
SRC="${1:-}"
if [[ -z "$SRC" ]]; then
    if [[ -x "./build/macos-rdp-daemon" ]]; then
        SRC="./build/macos-rdp-daemon"
    else
        echo "No binary given and ./build/macos-rdp-daemon not found." >&2
        echo "Build first, or: scripts/install-user.sh /path/to/macos-rdp-daemon" >&2
        exit 1
    fi
fi
[[ -f "$SRC" ]] || { echo "Binary not found: $SRC" >&2; exit 1; }

mkdir -p "$BIN_DIR" "$SIGN_DIR" "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"

# 2. Stable self-signed code-signing cert (created once, reused on every rebuild) -------
if [[ ! -f "$SIGN_DIR/cert.pem" ]]; then
    echo "==> Generating stable self-signed code-signing certificate..."
    cat > "$SIGN_DIR/openssl.cnf" <<'CNF'
[req]
distinguished_name = dn
x509_extensions    = v3
prompt             = no
[dn]
CN = macos-rdp-signing
[v3]
basicConstraints   = critical,CA:false
keyUsage           = critical,digitalSignature
extendedKeyUsage   = critical,codeSigning
CNF
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$SIGN_DIR/key.pem" -out "$SIGN_DIR/cert.pem" \
        -config "$SIGN_DIR/openssl.cnf"
    openssl pkcs12 -export -name "$CN" \
        -inkey "$SIGN_DIR/key.pem" -in "$SIGN_DIR/cert.pem" \
        -out "$SIGN_DIR/cert.p12" -passout "pass:$P12PASS"
fi

# 3. Keychain holding the signing identity (idempotent, unlocked, no auto-lock) ---------
security delete-keychain "$KC" 2>/dev/null || true
security create-keychain -p "$KCPASS" "$KC"
security set-keychain-settings "$KC"
security unlock-keychain -p "$KCPASS" "$KC"
security list-keychains -d user -s "$KC" "$HOME/Library/Keychains/login.keychain-db"
security import "$SIGN_DIR/cert.p12" -k "$KC" -P "$P12PASS" -T /usr/bin/codesign -A
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$KCPASS" "$KC" >/dev/null 2>&1 || true

HASH="$(security find-certificate -c "$CN" -Z "$KC" | awk '/SHA-1/{print $3; exit}')"
[[ -n "$HASH" ]] || { echo "Could not determine signing cert hash" >&2; exit 1; }

# 4. Install + sign the binary (stable DR -> TCC grant survives rebuilds) ---------------
echo "==> Installing and signing binary..."
cp "$SRC" "$BIN"
chmod 755 "$BIN"
# The cert is untrusted for Gatekeeper, but TCC matches the Designated Requirement (not
# Gatekeeper trust) and launchd runs it regardless, so this is all we need.
codesign --keychain "$KC" -s "$HASH" --identifier macos-rdp-daemon --force "$BIN"
codesign -d -r- "$BIN" 2>&1 | grep -i "designated" || true

# 5. RDP TLS certificate (self-signed) into the user cert dir ---------------------------
if [[ ! -f "$ROOT/server.crt" || ! -f "$ROOT/server.key" ]]; then
    echo "==> Generating RDP TLS certificate..."
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$ROOT/server.key" -out "$ROOT/server.crt" \
        -subj "/CN=$(hostname)"
    chmod 600 "$ROOT/server.key"
fi

# 6. Per-user LaunchAgent (Aqua session => WindowServer => capture works) ---------------
echo "==> Writing LaunchAgent..."
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key>
    <array><string>$BIN</string><string>--port</string><string>$PORT</string></array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>EnvironmentVariables</key>
    <dict>
        <key>RDP_CERT_DIR</key><string>$ROOT</string>
        <key>RDP_LOG_LEVEL</key><string>info</string>
    </dict>
    <key>StandardOutPath</key><string>$HOME/Library/Logs/macos-rdp-agent.log</string>
    <key>StandardErrorPath</key><string>$HOME/Library/Logs/macos-rdp-agent.error.log</string>
    <key>ThrottleInterval</key><integer>5</integer>
    <key>ProcessType</key><string>Interactive</string>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
</dict>
</plist>
PL

# 7. (Re)load the agent -----------------------------------------------------------------
launchctl bootout "$GUI/$LABEL" 2>/dev/null || true
launchctl enable "$GUI/$LABEL" 2>/dev/null || true   # clear any prior `disable` override
launchctl bootstrap "$GUI" "$PLIST"

cat <<EOF

Installed and running as $LABEL on port $PORT (user: $USER).

ONE-TIME permission grant (durable afterwards — survives rebuilds):
  1. Connect once from your RDP client to this Mac.
  2. Approve the Screen Recording + Accessibility prompts, or enable
     "macos-rdp-daemon" under System Settings > Privacy & Security.
  3. Reconnect — the desktop will render.

To deploy a new build later (no sudo, grant stays valid):
  scripts/install-user.sh /path/to/new/macos-rdp-daemon
EOF
