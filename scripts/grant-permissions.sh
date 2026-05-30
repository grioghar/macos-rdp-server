#!/bin/bash
# Grant — or guide granting — the Privacy permissions macos-rdp-daemon needs:
#   • Screen Recording  (CGDisplayStream frame capture)
#   • Accessibility     (CGEventPost keyboard/mouse injection)
#
# macOS stores these in a SIP-protected system database, so a fully automatic
# grant is only possible when System Integrity Protection is disabled. This
# script does the automatic grant when it can, and otherwise makes the manual
# step as close to one-click as macOS allows.
#
# Usage:  sudo bash grant-permissions.sh

set -uo pipefail   # intentionally not -e: this is best-effort, keep going

BINARY="/usr/local/sbin/macos-rdp-daemon"
TCC_SYS="/Library/Application Support/com.apple.TCC/TCC.db"
LABEL="com.macosrdp.daemon"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { echo -e "${GREEN}==>${NC} $*"; }
warn() { echo -e "${YELLOW}warn:${NC} $*"; }

[[ $EUID -ne 0 ]] && { echo -e "${RED}Run as root: sudo bash $0${NC}"; exit 1; }
[[ ! -x "$BINARY" ]] && { echo -e "${RED}$BINARY not found — install first.${NC}"; exit 1; }

REAL_USER="$(stat -f%Su /dev/console 2>/dev/null || echo "${SUDO_USER:-}")"

sip_on() { csrutil status 2>/dev/null | grep -qi "enabled"; }

# Best-effort direct grant via the system TCC database. Uses a named-column
# INSERT so it survives schema differences across macOS releases. Only succeeds
# when SIP is disabled (otherwise the DB is read-only even for root).
tcc_grant() {
    local service="$1"
    sqlite3 "$TCC_SYS" \
        "INSERT OR REPLACE INTO access
           (service, client, client_type, auth_value, auth_reason, auth_version, flags)
         VALUES ('$service', '$BINARY', 1, 2, 4, 1, 0);" 2>/dev/null
}

open_pane() {  # open a Privacy pane as the logged-in user (root can't open GUI)
    local anchor="$1"
    [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]] && \
        launchctl asuser "$(id -u "$REAL_USER")" sudo -u "$REAL_USER" \
            open "x-apple.systempreferences:com.apple.preference.security?$anchor" 2>/dev/null
}

# Return 0 if BOTH Screen Recording and Accessibility are already granted.
# Prefer the binary's own preflight check (official APIs, no Full Disk Access
# needed); only use it when the binary advertises the flag so an older build
# that ignores it and starts the server can't hang us. Else read the TCC DB.
perms_ok() {
    if "$BINARY" --help 2>&1 | grep -q -- "--check-permissions"; then
        "$BINARY" --check-permissions >/dev/null 2>&1
        return $?
    fi
    local s a
    s=$(sqlite3 "$TCC_SYS" "SELECT auth_value FROM access WHERE service='kTCCServiceScreenCapture' AND client='$BINARY' LIMIT 1;" 2>/dev/null)
    a=$(sqlite3 "$TCC_SYS" "SELECT auth_value FROM access WHERE service='kTCCServiceAccessibility'  AND client='$BINARY' LIMIT 1;" 2>/dev/null)
    [[ "$s" == "2" && "$a" == "2" ]]
}

echo ""
log "macos-rdp-daemon Privacy permissions helper"
echo ""

if perms_ok; then
    log "Screen Recording and Accessibility are already granted — nothing to do."
    launchctl kickstart -k "system/$LABEL" 2>/dev/null || true
    exit 0
fi

if sip_on; then
    warn "System Integrity Protection is ENABLED."
    warn "macOS will not let any tool grant Screen Recording / Accessibility"
    warn "directly — they must be toggled by hand. Opening the exact panes now."
    echo ""
    open_pane "Privacy_ScreenCapture"
    sleep 1
    open_pane "Privacy_Accessibility"

    cat <<EOF
  In the panes that just opened:
    1. Click the ${YELLOW}+${NC} button.
    2. Press ${YELLOW}Cmd-Shift-G${NC} and paste:  /usr/local/sbin
    3. Select ${YELLOW}macos-rdp-daemon${NC} and enable its switch.
  Do this for BOTH Screen Recording and Accessibility.

  Tip: if macos-rdp-daemon already appears in the list (it registers itself
  the first time an RDP client connects), just flip its switch on.

  Fully automatic granting requires disabling SIP — see docs/no-signing.md.
EOF
else
    warn "SIP is disabled — granting permissions directly."
    ok=1
    if tcc_grant "kTCCServiceScreenCapture"; then log "Granted: Screen Recording"; else warn "Screen Recording grant failed"; ok=0; fi
    if tcc_grant "kTCCServiceAccessibility"; then log "Granted: Accessibility";   else warn "Accessibility grant failed";   ok=0; fi
    if [[ $ok -eq 1 ]]; then
        log "Both permissions granted. Restarting the daemon…"
        launchctl kickstart -k "system/$LABEL" 2>/dev/null || true
    else
        warn "Opening the panes so you can finish manually."
        open_pane "Privacy_ScreenCapture"; sleep 1; open_pane "Privacy_Accessibility"
    fi
fi

echo ""
log "When both are enabled, (re)start the server:"
echo "    sudo launchctl kickstart -k system/$LABEL"
echo ""
