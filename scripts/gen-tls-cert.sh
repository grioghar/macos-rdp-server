#!/bin/bash
# Generate a self-signed TLS certificate for the RDP daemon.
# RDP uses TLS for the NLA/TLS security layer; clients will prompt to trust it.

set -euo pipefail

CERT_DIR="/etc/macos-rdp"
KEY="$CERT_DIR/server.key"
CERT="$CERT_DIR/server.crt"

mkdir -p "$CERT_DIR"
chmod 700 "$CERT_DIR"

HOST="$(hostname)"

# `-addext` requires OpenSSL 1.1.1+ / recent LibreSSL. macOS ships LibreSSL,
# where support varies, so fall back to a plain self-signed cert without a SAN
# if -addext is rejected. A self-signed RDP cert is trusted on first connect
# regardless of SAN, so this is functionally equivalent.
if ! openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
        -keyout "$KEY" -out "$CERT" \
        -subj "/CN=$HOST/O=macOS RDP/C=US" \
        -addext "subjectAltName=DNS:$HOST,IP:127.0.0.1" 2>/dev/null; then
    echo "note: openssl -addext unsupported; generating certificate without SAN"
    openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
        -keyout "$KEY" -out "$CERT" \
        -subj "/CN=$HOST/O=macOS RDP/C=US"
fi

chmod 600 "$KEY"
chmod 644 "$CERT"

echo "Generated TLS certificate:"
openssl x509 -in "$CERT" -noout -subject -dates -fingerprint -sha256
