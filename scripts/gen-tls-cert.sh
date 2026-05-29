#!/bin/bash
# Generate a self-signed TLS certificate for the RDP daemon.
# RDP uses TLS for the NLA/TLS security layer; clients will prompt to trust it.

set -euo pipefail

CERT_DIR="/etc/macos-rdp"
KEY="$CERT_DIR/server.key"
CERT="$CERT_DIR/server.crt"

mkdir -p "$CERT_DIR"
chmod 700 "$CERT_DIR"

openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
    -keyout "$KEY" \
    -out    "$CERT" \
    -subj "/CN=$(hostname)/O=macOS RDP/C=US" \
    -addext "subjectAltName=DNS:$(hostname),IP:127.0.0.1"

chmod 600 "$KEY"
chmod 644 "$CERT"

echo "Generated TLS certificate:"
openssl x509 -in "$CERT" -noout -subject -dates -fingerprint -sha256
