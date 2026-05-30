# macos-rdp-server

A lightweight RDP server daemon for macOS Tahoe and later. Connect to your Mac from any standard RDP client — Windows Remote Desktop, FreeRDP, Remmina — with the same smoothness you'd expect from a Windows environment.

## Features

- **H.264 hardware encoding** via VideoToolbox — low latency, low CPU
- **CGVirtualDisplay** — creates a dedicated virtual display for each session; your physical screen is undisturbed
- **Full keyboard & mouse injection** via CGEventPost with a complete RDP scan-code table
- **Bidirectional clipboard** — text and images sync between client and host
- **System audio redirection** — hear your Mac's audio through the RDP client
- **Structured logging** — four levels (ERROR / INFO / VERBOSE / DEBUG), set via flag or env var
- **DriverKit HID extension** *(optional, requires Apple Developer account)* — injects input at the HID stack level, works at the login window and in secure input fields
- **launchd integration** — auto-starts at boot, restarts on crash
- **TLS encryption** — self-signed certificate generated on install; clients trust on first connect

## Requirements

- macOS 14 Sonoma or later
- Homebrew (only needed when building from source; pre-built binaries have no runtime dependencies)
- Xcode Command Line Tools (source builds only)

## One-line install

```bash
curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/master/scripts/remote-install.sh | sudo bash
```

Downloads the latest pre-built **universal binary** (a single file that runs natively on both Intel and Apple Silicon) from GitHub Releases. No Homebrew, no compilation, no runtime dependencies — the binary links only against macOS system frameworks.

The script will:
1. Download the universal binary to `/usr/local/sbin/macos-rdp-daemon`
2. Generate a self-signed TLS certificate at `/etc/macos-rdp/`
3. Install and load the launchd service on port 3389
4. Print your IP address and the two Privacy permission steps required

Or download the binary directly:
```bash
sudo curl -fsSL https://github.com/grioghar/macos-rdp-server/releases/latest/download/macos-rdp-daemon \
  -o /usr/local/sbin/macos-rdp-daemon && sudo chmod +x /usr/local/sbin/macos-rdp-daemon
```

Verify the download against the published checksum:
```bash
curl -fsSL https://github.com/grioghar/macos-rdp-server/releases/latest/download/SHA256SUMS | shasum -c
```

After installation, open any RDP client and connect to your Mac's IP address.

> **Reproducible builds.** Release binaries are built entirely from source-pinned forks
> (FreeRDP and OpenSSL), statically linked, in a single CI pass. Nothing is pulled from a
> package manager at build or run time. See [`.github/workflows/release.yml`](.github/workflows/release.yml).

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

| Permission | Required for |
|---|---|
| Screen Recording | `CGDisplayStream` frame capture |
| Accessibility | `CGEventPost` keyboard & mouse injection |
| Microphone | CoreAudio system audio tap |

```bash
# Or pre-grant from an admin terminal (requires SIP off):
sudo bash scripts/grant-permissions.sh
```

## Configuration

### Port

Edit `/Library/LaunchDaemons/com.macosrdp.daemon.plist`:

```bash
sudo launchctl unload /Library/LaunchDaemons/com.macosrdp.daemon.plist
# set --port <n> in ProgramArguments
sudo launchctl load -w /Library/LaunchDaemons/com.macosrdp.daemon.plist
```

| Flag | Default | Description |
|---|---|---|
| `--port` | `3389` | TCP port to listen on |
| `--log-level` | `info` | Log verbosity (see below) |

## Logging

The daemon uses a four-level structured logger. Each log line includes a timestamp, level tag, and subsystem component:

```
[14:23:01.042] [INFO ] [server] listening for RDP connections on port 3389
[14:23:04.187] [INFO ] [main  ] client connected from ::ffff:192.168.1.5
[14:23:04.201] [INFO ] [peer  ] peer activated: 1920x1080 @32bpp
[14:23:04.203] [INFO ] [session] setting up display 1920x1080 for ::ffff:192.168.1.5
[14:23:04.251] [INFO ] [display] virtual display created: displayID=3 1920x1080
[14:23:04.260] [INFO ] [encoder] H.264 encoder ready: 1920x1080 @ 8000 kbps
[14:23:04.261] [INFO ] [capture] capture started on displayID=3
[14:23:04.265] [INFO ] [audio  ] audio capture started on device 48
[14:23:04.266] [INFO ] [session] session active for ::ffff:192.168.1.5
```

### Log levels

| Level | What it logs | Use when |
|---|---|---|
| `error` | Hard failures that stop a session or the daemon | Production; quiet machines |
| `info` | Connection lifecycle — connect, activate, disconnect | Default |
| `verbose` | Subsystem events — channel opens, display creation, encoder start, audio start | Diagnosing connection or feature issues |
| `debug` | Per-frame and per-event detail — every keypress, every encoded frame size, every dirty rect | Deep protocol troubleshooting |

> **Warning:** `debug` logs every captured frame and input event. At 60fps this is ~3,600 lines/minute. Use it for short captures only.

### Setting the log level

**CLI flag** (one-off / manual runs):
```bash
sudo macos-rdp-daemon --log-level verbose
sudo macos-rdp-daemon --log-level debug
```

**Environment variable** (persists across restarts):
```bash
# Live, without editing the plist:
sudo launchctl setenv RDP_LOG_LEVEL verbose
sudo launchctl kickstart -k system/com.macosrdp.daemon

# Or set it permanently in the launchd plist:
# /Library/LaunchDaemons/com.macosrdp.daemon.plist
# → EnvironmentVariables → RDP_LOG_LEVEL
```

### Reading logs

```bash
# Follow the daemon log file directly:
tail -f /var/log/macos-rdp-daemon.error.log

# Filter by level:
tail -f /var/log/macos-rdp-daemon.error.log | grep '\[ERROR\]'

# Via macOS unified logging (also shows os_log entries):
log stream --predicate 'process == "macos-rdp-daemon"' --level debug

# Historical logs in Console.app:
# Filter by process name "macos-rdp-daemon"
```

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
│         ClipboardSync      RDPLog                         │
│        (NSPasteboard)   (ERROR/INFO/VERBOSE/DEBUG)        │
└─────────────────────────────────────────────────────────┘

Optional DriverKit HID extension (requires Apple Developer account):
injects keyboard/mouse at the HID level — works at login window
```

## Without an Apple Developer account

The daemon runs fully without code signing. You only need a Developer account for the optional DriverKit HID extension. See [Running without signing](docs/no-signing.md).

## License

MIT
