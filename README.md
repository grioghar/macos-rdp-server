# macos-rdp-server

A lightweight RDP server daemon for macOS Tahoe and later. Connect to your Mac from any standard RDP client — Windows Remote Desktop, FreeRDP, Remmina — with the same smoothness you'd expect from a Windows environment.

## Features

- **H.264 hardware encoding** via VideoToolbox — low latency, low CPU
- **CGVirtualDisplay** — creates a dedicated virtual display for each session; your physical screen is undisturbed
- **Full keyboard & mouse injection** via CGEventPost with a complete RDP scan-code table
- **Bidirectional clipboard** — text and images sync between client and host
- **System audio redirection** — hear your Mac's audio through the RDP client
- **DriverKit HID extension** *(optional, requires Apple Developer account)* — injects input at the HID stack level, works at the login window and in secure input fields
- **launchd integration** — auto-starts at boot, restarts on crash
- **TLS encryption** — self-signed certificate generated on install; clients trust on first connect

## Requirements

- macOS 15 Sequoia or later (macOS 14 Sonoma works but is untested)
- Homebrew
- Xcode Command Line Tools

## One-line install

```bash
curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/main/scripts/remote-install.sh | sudo bash
```

This will:
1. Install Homebrew dependencies (`freerdp3`, `cmake`, `openssl`)
2. Clone this repository to `/usr/local/src/macos-rdp-server`
3. Build the daemon
4. Generate a self-signed TLS certificate
5. Install and load the launchd service on port 3389

After installation, open **Remote Desktop** (Windows / macOS / iOS) or any RDP client and connect to your Mac's IP address.

## Manual install

```bash
# 1. Install dependencies
brew install freerdp cmake openssl

# 2. Clone
git clone https://github.com/grioghar/macos-rdp-server.git
cd macos-rdp-server

# 3. Build & install
sudo bash scripts/install.sh
```

## Connect

| Client | Steps |
|---|---|
| Windows Remote Desktop | Start → `mstsc` → enter your Mac's IP |
| macOS Remote Desktop | App Store → Microsoft Remote Desktop → Add PC |
| FreeRDP (CLI) | `xfreerdp /v:your-mac-ip /u:$(whoami) /cert:ignore` |
| Remmina | New connection → protocol RDP → enter IP |

On first connect your client will show a certificate trust prompt — accept it. Subsequent connections skip this.

## Privacy permissions

On first run, macOS will block Screen Recording and Accessibility access. Grant both in **System Settings → Privacy & Security**:

```bash
# Or pre-grant from an admin terminal (requires SIP off):
sudo bash scripts/grant-permissions.sh
```

## Configuration

Edit `/Library/LaunchDaemons/com.macosrdp.daemon.plist` to change the port or add flags:

```bash
sudo launchctl unload /Library/LaunchDaemons/com.macosrdp.daemon.plist
# edit the plist
sudo launchctl load -w /Library/LaunchDaemons/com.macosrdp.daemon.plist
```

| Flag | Default | Description |
|---|---|---|
| `--port` | `3389` | TCP port to listen on |

## Uninstall

```bash
sudo bash scripts/uninstall.sh
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                  macos-rdp-daemon (root)                 │
│                                                          │
│  TCP :3389 ──► RDPServer ──► RDPSession (per client)    │
│                                   │                      │
│              ┌────────────────────┼──────────────────┐   │
│              │                   │                   │   │
│         RDPPeer              VirtualDisplay    AudioCapture
│      (libfreerdp 3)        (CGVirtualDisplay) (CoreAudio HAL)
│              │                   │                   │   │
│         GFX Pipeline       ScreenCapture       AudioRedirect
│       (H.264 AVC420)     (CGDisplayStream)     (RDPSND ch.)
│              │                   │                        │
│         InputInjector       FrameEncoder                  │
│        (CGEventPost)      (VideoToolbox)                  │
│              │                                            │
│         ClipboardSync                                     │
│        (NSPasteboard)                                     │
└─────────────────────────────────────────────────────────┘

Optional DriverKit HID extension (requires Apple Developer account):
injects keyboard/mouse at the HID level — works at login window
```

## Without an Apple Developer account

The daemon runs fully without code signing. You only need a Developer account for the optional DriverKit HID extension. See [Running without signing](docs/no-signing.md).

## License

MIT
