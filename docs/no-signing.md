# Running without an Apple Developer account

The daemon (`macos-rdp-daemon`) runs fully without code signing. You only need a Developer account for the optional DriverKit HID extension.

## What works without signing

| Feature | Works without signing |
|---|---|
| Screen capture (CGDisplayStream) | Yes |
| Virtual display (CGVirtualDisplay) | Yes, when running as root |
| Keyboard & mouse injection (CGEventPost) | Yes, with Accessibility permission |
| Clipboard sync | Yes |
| Audio redirection | Yes, when running as root |
| TLS encryption | Yes (self-signed certificate) |
| DriverKit HID extension | **No** — requires signing |

The DriverKit HID extension is a quality enhancement. Without it, input injection uses `CGEventPost`, which works for every app except those with macOS Secure Keyboard Entry enabled (Terminal, 1Password, etc.). For most RDP use cases this is irrelevant.

## Quick setup without signing

```bash
# Install
curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/main/scripts/remote-install.sh | sudo bash

# Grant Privacy permissions (easiest: System Settings → Privacy & Security)
# Or if SIP is disabled:
sudo bash /usr/local/src/macos-rdp-server/scripts/grant-permissions.sh
```

## Disable SIP (optional, for grant-permissions.sh)

Disabling SIP is not required for normal operation. It only helps if you want to skip the System Settings permission prompts.

1. Restart into Recovery Mode: hold **Power** (Apple Silicon) or **⌘R** (Intel) at boot
2. Open Terminal from the Utilities menu
3. Run: `csrutil disable`
4. Restart

To re-enable: same steps but `csrutil enable`.

A less invasive option that still allows TCC DB writes:
```
csrutil enable --without-dtrace
```
