# macOS RDP Server — Feature & Stability Test Plan

Goal: parity with the Windows Remote Desktop (mstsc) experience, and rock-solid stability.

Status legend: ✅ working · ⚠️ partial / needs work · ❌ not implemented · ❓ untested

Test client: Windows `mstsc` → macOS Tahoe 26 arm64. Server build auto-updates from master CI.

---

## 1. Connection & Security
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 1.1 | TCP connect on 3389 | `mstsc /v:192.168.1.152` | Session opens | ✅ |
| 1.2 | TLS negotiation | Connect; accept cert | Encrypted session | ✅ |
| 1.3 | Self-signed cert prompt | First connect | Cert warning, then connects | ✅ |
| 1.4 | Auth — correct creds | Enter Mac username+password | Authenticates, session starts | ✅ |
| 1.5 | Auth — wrong creds | Enter wrong password | Rejected, no session | ✅ |
| 1.6 | Auth — session takeover | Connect 2nd client with correct creds | 2nd client takes over | ✅ |
| 1.7 | Auth — takeover with wrong creds | Connect 2nd client with bad creds | Rejected, 1st session undisturbed | ❓ |
| 1.8 | NLA disabled | Try NLA-required client | NLA is off server-side (document) | ⚠️ |
| 1.9 | Durable TCC grant across rebuilds | Auto-update reloads daemon | Screen Recording still granted | ✅ |
| 1.10 | Grant across reboot | Reboot Mac, reconnect | TBD — non-notarized binary | ❓ |
| 1.11 | Multi-monitor client | Connect mstsc with "Use all my monitors" | Single combined virtual display | ❓ |

## 2. Display
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 2.1 | Desktop renders (AVC420/H.264) | Connect | Live desktop, correct content | ✅ |
| 2.2 | Virtual display at client resolution | Connect at 3440×1440 | No pillarbox, native 1:1 | ✅ |
| 2.3 | Correct colors | View photos / gradients | Accurate color | ❓ |
| 2.4 | Refresh on minimize/restore | Minimize then restore mstsc | Repaints cleanly | ✅ (SuppressOutput + IDR) |
| 2.5 | Frame rate / smoothness | Drag a window, scroll | Fluid ~30 fps | ❓ |
| 2.6 | Dynamic resolution change | Resize mstsc window | Session adapts | ❓ |
| 2.7 | Non-ultrawide client (1920×1080) | Connect from 16:9 | Correct proportions | ❓ |
| 2.8 | Privacy blank on connect | Set RDP_PRIVACY_BLANK=1 | Local display dims/blanks | ⚠️ (DisplayServices brightness only) |
| 2.9 | Shared mode | Set RDP_SHARED_MODE=1 | Remote + local see same screen | ❓ |
| 2.10 | Wake Mac on connect | Mac display asleep, then connect | Display wakes, capture starts | ✅ (IOPMAssertion) |

## 3. Input
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 3.1 | Mouse move (1:1 position) | Move across whole screen | Tracks accurately | ✅ |
| 3.2 | Cursor shapes (I-beam, resize, spinner) | Hover text / window edge | Shows correct cursor | ✅ (RDP_CURSOR_SHAPES=1, 24bpp) |
| 3.3 | Left/right/middle click | Click around | Registers correctly | ❓ |
| 3.4 | Click-drag | Drag a window / select text | Works | ❓ |
| 3.5 | Scroll wheel (vert/horiz) | Scroll a page | Scrolls correct direction | ❓ |
| 3.6 | Keyboard — letters/numbers/symbols | Type in TextEdit | Correct chars | ❓ |
| 3.7 | Modifiers (⌘ ⌥ ⌃ ⇧) + shortcuts | ⌘C / ⌘V / ⌘Tab / ⌘Space | Work | ❓ |
| 3.8 | Special keys (F1-F12, Esc, arrows, Del) | Press them | Work | ❓ |

## 4. Clipboard (CLIPRDR)
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 4.1 | Text Mac → Windows | Copy on Mac, paste on Win | Pastes correctly | ✅ |
| 4.2 | Text Windows → Mac | Copy on Win, paste on Mac | Pastes correctly | ✅ |
| 4.3 | Unicode / emoji / RTL text | Copy/paste 非ASCII 🎉 | Preserved | ✅ |
| 4.4 | Large text | Copy a large block | No truncation | ✅ |
| 4.5 | Image (PNG) Mac → Windows | Copy a screenshot on Mac, paste in Paint | Image appears | ❓ (0xC004 named format) |
| 4.6 | Image Windows → Mac | Copy image on Win, paste in Preview | Image appears | ❓ |
| 4.7 | File drag/drop | Drag a file | Not implemented (HDROP) | ❌ |

## 5. Audio
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 5.1 | System audio out (Mac → client) | Play music on Mac | Hear it on Windows headphones | ✅ (CATap, 44.1 kHz) |
| 5.2 | Audio pitch / rate | Play a 440 Hz tone | Correct pitch | ✅ |
| 5.3 | Audio + local speakers | RDP_AUDIO_LOCAL=1 | Plays on both Mac+client | ✅ |
| 5.4 | Audio quality / sync | Play a video | Clear, in sync | ❓ |
| 5.5 | Mic input (Windows → Mac speakers) | RDP_AUDIO_INPUT=1, Win "Record from this computer" | Mac plays Windows mic | ✅ (MS-RDPEAI, 16kHz PCM) |

## 6. Drive Redirection (RDPDR / WebDAV)
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 6.1 | Channel open | Connect with Drives enabled in mstsc | Log shows "rdpdr: channel open" | ✅ |
| 6.2 | Drive enumeration | Connect with C: shared | Log shows "rdpdr: client drive C" | ✅ |
| 6.3 | WebDAV server starts | See above | Log shows "webdav: listening on :876x" | ❓ needs test |
| 6.4 | Volume mounts in Finder | See above | /Volumes/RDP-C appears in Finder | ❓ needs hardware test |
| 6.5 | Browse files (PROPFIND) | Open /Volumes/RDP-C in Finder | Windows files visible | ❓ |
| 6.6 | Read file (GET) | Open a text file | Contents correct | ❓ |
| 6.7 | Write file (PUT) | Save a file to /Volumes/RDP-C | File appears on Windows | ❓ |
| 6.8 | Delete file | Trash a file | Deleted on Windows | ❓ |
| 6.9 | Unmount on disconnect | Disconnect mstsc | /Volumes/RDP-C disappears | ❓ |
| 6.10 | Printer / serial / USB | — | Not built | ❌ |

**How to enable:** `RDP_RDPDR_ENABLED=1` must be in the LaunchAgent plist (set by default).  
**mstsc setup:** Options → Local Resources → More → Drives → check your Windows drives.

## 7. Auto-Update
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 7.1 | Polls GitHub releases | Check daemon log | "up to date (version XXXXXXX)" every 5 min | ✅ |
| 7.2 | Updates on new push | Push a commit | Daemon swaps and restarts within 5 min | ✅ |
| 7.3 | Defers while client connected | Be connected when update is available | Log "deferring: client active" | ✅ |
| 7.4 | Re-signs after download | Inspect code signature post-update | Valid signature | ✅ |
| 7.5 | SHA-256 integrity check | — | Daemon verifies hash before applying | ✅ |

## 8. Session management
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 8.1 | Clean disconnect | Close mstsc | Server tears down, no leak | ✅ |
| 8.2 | Reconnect after disconnect | Reconnect | Fresh session works | ✅ |
| 8.3 | Negotiation watchdog | Half-open connection stuck | Drops after 20s | ✅ |
| 8.4 | Multiple sequential sessions | Connect/disconnect ×10 | Each works, no leak | ❓ |
| 8.5 | Two simultaneous clients | Connect from 2 machines | 2nd must auth; takeover or reject | ✅ |
| 8.6 | Idle session (10 min) | Leave idle | Stays up, low CPU | ❓ |
| 8.7 | Display restore on disconnect | Disconnect | Built-in display restored | ❓ |

## 9. Performance
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 9.1 | Idle CPU | Connected, static screen | Low (heartbeat ~4 fps) | ❓ |
| 9.2 | Active CPU | Drag windows / play video | Reasonable | ❓ |
| 9.3 | Input latency | Move mouse / type | Snappy (<50ms) | ✅ (cursor) / ❓ (keys) |
| 9.4 | Memory (no leak) | Watch RSS over 1h | Flat | ❓ |

## 10. Stability / soak
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 10.1 | Long soak (1–8 h) | Keep session open | No drop, no leak, no crash | ❓ |
| 10.2 | Rapid reconnect ×20 | Connect/disconnect fast | No crash / port stuck | ❓ |
| 10.3 | Network blip | Briefly drop Wi-Fi | Recovers or clean fail | ❓ |
| 10.4 | Mac sleep/wake | Sleep the Mac, wake, reconnect | Recovers (IOPMAssertion prevents sleep during session) | ❓ |
| 10.5 | Agent auto-restart | `launchctl kill -TERM gui/501/com.macosrdp.agent.user` | launchd restarts it in 5s | ✅ |

## 11. Install & configuration
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 11.1 | User installer | Run scripts/install-user.sh on Mac | Installs, signs, loads | ⚠️ untested end-to-end |
| 11.2 | GUI config app | Open the menu-bar app | Toggle all options, start/stop | ✅ (gui/build.sh) |
| 11.3 | Grant permissions | scripts/grant-permissions.sh | Screen Recording + Accessibility granted | ⚠️ |
| 11.4 | Uninstall | scripts/uninstall.sh | Clean removal | ❓ |
| 11.5 | Remote install (fresh Mac) | scripts/remote-install.sh | Binary + plist + cert installed | ⚠️ untested |

---

### Environment variable reference

| Variable | Default | Description |
|---|---|---|
| `RDP_LOG_LEVEL` | info | Log verbosity: error / info / verbose / debug |
| `RDP_CERT_DIR` | *required* | Directory containing server.crt + server.key |
| `RDP_CURSOR_SHAPES` | 1 | Send real Mac cursor shapes (24bpp) |
| `RDP_SHOW_CURSOR` | 0 | Overlay Mac cursor in the video stream |
| `RDP_SHARED_MODE` | 0 | Allow local+remote to interact simultaneously |
| `RDP_PRIVACY_BLANK` | 0 | Dim built-in display brightness on connect |
| `RDP_RDPDR_ENABLED` | 1 | Enable Windows drive redirection |
| `RDP_AUDIO_LOCAL` | 1 | Also play audio on local Mac speakers |
| `RDP_AUDIO_INPUT` | 0 | Receive Windows mic → Mac speakers (MS-RDPEAI) |
| `RDP_ALLOW_IDLE_SLEEP` | 0 | Allow Mac to idle-sleep (breaks remote access) |
| `RDP_UPDATE_ENABLED` | 1 | Enable auto-update |
| `RDP_UPDATE_INTERVAL_MIN` | 5 | Auto-update check interval in minutes |
| `RDP_SIGN_IDENTITY` | — | Codesign identity SHA-1 for re-signing after update |
| `RDP_SIGN_KEYCHAIN` | — | Keychain path for codesigning |
| `RDP_SIGN_KEYCHAIN_PW` | — | Keychain unlock password |

### Known issues / in progress
1. **Drive mounting needs hardware test** — WebDAV bridge is built; test by connecting mstsc with Drives enabled.
2. **Image clipboard** — PNG support is wired (0xC004), but needs verification against real mstsc.
3. **Stability soak** — §10 scenarios not run yet.
4. **Multi-monitor per-display** — span mode works (one big virtual display); per-display virtual displays not built.
5. **Printer/USB redirection** — out of scope for personal use.
6. **Upstream FreeRDP PRs** — 1 PERF + 2 MEDIUM fixes ready to file (commands in docs/security-audit-2026-06-02.md).
