## macos-rdp-server VERSION

Lightweight RDP server for macOS. Single universal binary — runs natively on Intel and Apple Silicon. No Homebrew, no compilation.

### One-line install

    curl -fsSL https://raw.githubusercontent.com/grioghar/macos-rdp-server/main/scripts/remote-install.sh | sudo bash

### Manual binary download

    sudo curl -fsSL https://github.com/grioghar/macos-rdp-server/releases/download/VERSION/macos-rdp-daemon -o /usr/local/sbin/macos-rdp-daemon
    sudo chmod +x /usr/local/sbin/macos-rdp-daemon

### Verify checksum

    curl -fsSL https://github.com/grioghar/macos-rdp-server/releases/download/VERSION/SHA256SUMS | shasum -c

### What's included

- H.264 hardware encoding via VideoToolbox
- Screen capture via CGDisplayStream (zero-copy IOSurface path)
- System audio redirection via CoreAudio HAL
- Bidirectional clipboard (text + images)
- Full keyboard and mouse injection
- TLS encryption (self-signed cert generated on first run)
- Zero idle CPU: frames skipped on static screens, event-driven signal handling

### Built with

- FreeRDP FREERDP_VER (statically linked, no runtime dependencies)
- macOS 14 SDK, universal binary (arm64 + x86_64)
