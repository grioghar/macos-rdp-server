# macOS RDP Server — Feature & Stability Test Plan

Goal: parity with the Windows Remote Desktop (mstsc) experience, and rock-solid stability.

Status legend: ✅ working · ⚠️ partial / needs work · ❌ not implemented · ❓ untested

Test client: Windows `mstsc` (3440×1440 ultrawide) → macOS Tahoe 26 (built-in Retina 3024×1964).

---

## 1. Connection & Security
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 1.1 | TCP connect on 3389 | Connect with mstsc | Session opens | ✅ |
| 1.2 | TLS negotiation | Connect; accept cert | Encrypted, no downgrade | ✅ |
| 1.3 | Self-signed cert prompt | First connect | Cert warning, then connects | ✅ |
| 1.4 | Auth (username/password) | Enter Mac creds | Authenticates | ✅ |
| 1.5 | NLA | Try with NLA required client | (NLA off server-side) — document | ❓ |
| 1.6 | Durable Screen-Recording grant across rebuilds | Redeploy a new build, reconnect | No re-grant needed (cert-pinned) | ✅ |
| 1.7 | Grant across **reboot** | Reboot Mac, reconnect | TBD — may need re-grant (non-notarized) | ❓ |

## 2. Display
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 2.1 | Desktop renders (AVC420/H.264) | Connect | Live desktop | ✅ |
| 2.2 | Virtual display at client resolution | Connect at 3440×1440 | No pillarbox, native | ✅ |
| 2.3 | Correct colors (no tint/banding) | View photos/gradients | Accurate color | ❓ |
| 2.4 | Full-surface refresh on big changes | Open/close/minimize windows | Repaints cleanly | ⚠️ (earlier "minimize/restore" refresh report — re-test) |
| 2.5 | Frame rate / smoothness | Drag a window, play video | Fluid ~30fps | ❓ |
| 2.6 | Multi-monitor | Connect with 2 client monitors | N/A (single vdisp) | ❌ |
| 2.7 | Dynamic resolution change | Resize mstsc window / reconnect at new size | Adapts | ❓ |
| 2.8 | Non-ultrawide client (e.g. 1920×1080) | Connect from a 16:9 client | Correct, no distortion | ❓ |

## 3. Input
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 3.1 | Mouse move (1:1 position) | Move across whole screen | Tracks correctly | ✅ |
| 3.2 | Cursor smoothness | Move quickly | Smooth (client-side) | ✅ |
| 3.3 | Cursor **shapes** (I-beam, resize, beachball) | Hover text/edges | Shows correct shape | ❌ (always arrow — color-pointer PDUs TODO) |
| 3.4 | Left/right/middle click | Click around | Registers correctly | ❓ |
| 3.5 | Click-drag (select text, move window) | Drag | Works | ❓ |
| 3.6 | Scroll wheel (vert/horiz) | Scroll a page | Scrolls correct direction | ❓ |
| 3.7 | Keyboard — letters/numbers | Type in TextEdit | Correct chars | ❓ |
| 3.8 | Modifiers (⌘ ⌥ ⌃ ⇧) + shortcuts (⌘C/⌘V/⌘Tab) | Use shortcuts | Work | ❓ |
| 3.9 | Special keys (arrows, F-keys, Esc, Del, Return) | Use them | Work | ❓ |
| 3.10 | Key mapping (Windows keyboard → Mac) | Test ⌘ vs Ctrl placement | Sensible mapping | ❓ |

## 4. Clipboard (CLIPRDR)
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 4.1 | Text Mac → Windows | Copy on Mac, paste on Win | Pastes text | ❓ |
| 4.2 | Text Windows → Mac | Copy on Win, paste on Mac | Pastes text | ❓ |
| 4.3 | Unicode / emoji | Copy/paste非ASCII | Preserved | ❓ |
| 4.4 | Large text | Copy a big block | No truncation | ❓ |
| 4.5 | Image copy/paste | Copy an image | Works (or document unsupported) | ❓ |
| 4.6 | File copy/paste | Copy a file | (likely unsupported) | ❓ |

## 5. Audio
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 5.1 | System audio out (Mac → client) | Play music/video on Mac | Hear it on Windows | ⚠️ **0 frames captured — needs CoreAudio process-tap (CATap, macOS 14.4+)** |
| 5.2 | Audio quality / sync | Play a video | Clear, in sync | ❓ |
| 5.3 | Mic input (client → Mac) | Speak into Win mic | Mac receives (AUDIN) | ❌ not built |

## 6. File sharing / device redirection (RDPDR)
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 6.1 | Client drive redirection (Windows drives on Mac) | Enable "Drives" in mstsc | Mac sees the drives | ❌ RDPDR disabled in build (big lift on macOS) |
| 6.2 | Printer redirection | — | — | ❌ not built |
| 6.3 | Smart card / serial / USB | — | — | ❌ not built (scope) |

## 7. Session management
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 7.1 | Clean disconnect | Close mstsc | Server tears down, frees resources | ✅ |
| 7.2 | Reconnect after disconnect | Reconnect | Fresh session works | ✅ |
| 7.3 | No 0x1108 auto-reconnect loop | Drop & reconnect | Reconnects cleanly | ✅ (autoreconnect off in .rdp) |
| 7.4 | Multiple sequential sessions | Connect/disconnect ×10 | Each works, no leak | ❓ |
| 7.5 | Two simultaneous clients | Connect from 2 machines | Defined behavior (reject or share) | ❓ |
| 7.6 | Idle session | Leave idle 10 min | Stays up, low CPU | ❓ |
| 7.7 | Display restore on disconnect | Disconnect | Built-in display restored as main | ❓ (virtual display set main for session) |

## 8. Performance
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 8.1 | Idle CPU (server) | Connected, static screen | Low (heartbeat ~4fps) | ❓ |
| 8.2 | Active CPU (server) | Drag windows/video | Reasonable | ❓ |
| 8.3 | Input latency | Move mouse / type | Snappy | ✅ (cursor) / ❓ (keys) |
| 8.4 | Bandwidth | Monitor during video | Bounded (~8 Mbps cap) | ❓ |

## 9. Stability / soak
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 9.1 | Long soak (1–8 h) | Keep a session open | No drop, no leak, no crash | ❓ |
| 9.2 | Rapid reconnect ×20 | Connect/disconnect fast | No crash / port stuck | ❓ |
| 9.3 | Network blip | Briefly drop Wi-Fi | Recovers or clean fail | ❓ |
| 9.4 | Mac sleep/wake | Sleep the Mac, wake, reconnect | Recovers | ❓ |
| 9.5 | Memory/FD leak | Watch RSS / lsof over time | Flat | ❓ |
| 9.6 | Agent auto-restart (KeepAlive) | `kill` the agent | launchd restarts it | ✅ |

## 10. Install & configuration
| # | Item | How to test | Expected | Status |
|---|------|-------------|----------|--------|
| 10.1 | Durable user installer | Run scripts/install-user.sh | Installs, signs, loads | ⚠️ untested end-to-end |
| 10.2 | One-time permission grant flow | Fresh install | Clear grant steps | ⚠️ |
| 10.3 | GUI config tool | — | Toggle cursor/display/audio, start/stop | ❌ planned |
| 10.4 | Uninstall | scripts/uninstall.sh | Clean removal | ❓ |

---

### Known issues / TODO (priority order)
1. **Audio capture = 0 frames** — implement CoreAudio process tap (CATap, macOS 14.4+) to capture system output.
2. **Cursor shapes** — send color-pointer PDUs from the captured Mac cursor (currently always arrow).
3. **File sharing** — RDPDR is a large lift on macOS (FUSE-style mount or a shared-folder bridge).
4. **GUI config tool** — menu-bar app to set options + start/stop.
5. **Soak/stability** — run §9 scenarios.
6. **Display restore** — restore the built-in as main display on session teardown.
