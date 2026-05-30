# Efficiency & Optimization Analysis

This document captures every identified optimization opportunity across the full stack —
our code, FreeRDP, and the macOS frameworks we use. It drives both in-tree improvements
and upstream PR candidates.

---

## Our code

### Already done
- **Event-driven peer loop** — replaced busy-poll with `WaitForMultipleObjects` over
  peer transport handles + VCM channel event + GFX channel event. Zero CPU between events.
- **`-Os` + LTO** — daemon and FreeRDP build with size optimization and link-time
  dead-stripping.
- **`MinSizeRel`** for FreeRDP — disables all features we don't call.
- **Zero-copy frame path** — `CGDisplayStream` delivers IOSurface; `CVPixelBufferCreateWithIOSurface`
  wraps it without copying; VideoToolbox H.264 encodes directly from it.

### Remaining in our code

| Area | Issue | Fix |
|------|-------|-----|
| `ScreenCapture.m` | Dirty rect is computed but H.264 path sends full-frame anyway | Wire dirty rect into a per-region `RDPGFX_SURFACE_COMMAND` to only encode/send changed tiles |
| `FrameEncoder.m` | One `VTCompressionSession` per resolution; resolution changes destroy+recreate | Use `VTCompressionSessionSetProperty(kVTCompressionPropertyKey_PixelTransferProperties)` to resize in-place |
| `ScreenCapture.m` | `kCGDisplayStreamMinimumFrameTime = 1/60` is hardcoded | Negotiate with client's `DesiredFrameRate` from RDP settings; idle sessions should drop to ~5fps |
| `AudioCapture.m` | IO proc fires on every output device callback even if no RDP client is active | Gate `captureBlock` on session-active flag; avoid even the float→int16 convert when idle |
| `ClipboardSync.m` | NSPasteboard polled every 500ms unconditionally | Use `NSPasteboardDidChange` distributed notification (macOS 10.15+) instead of polling |
| `RDPSession.m` | Audio starts even if client didn't request audio in capabilities | Check `FreeRDP_AudioPlayback` setting before starting `AudioCapture` |
| `daemon/main.m` | Run loop wakes every 1.0s to check `g_should_exit` | Replace with `kqueue` EVFILT_SIGNAL on SIGTERM/SIGINT — zero wakeups when idle |

---

## FreeRDP upstream opportunities

FreeRDP is **Apache 2.0** — contributions fully welcome.

### 1. `WTSVirtualChannelManagerCheckFileDescriptor` is a misnomer and polls

**File:** `libfreerdp/core/channels.c` (and wtsvc.c)  
**Issue:** The function is named `CheckFileDescriptor` but internally calls `WaitForSingleObject`
with timeout=0 — it's a non-blocking poll, not a proper event-driven check. Callers that want
to wait must spin-call it, burning CPU.  
**Fix:** Expose `WTSVirtualChannelManagerGetEventHandle` (already exists) more prominently in
docs/examples, and deprecate the `CheckFileDescriptor` variant in favor of a proper
`WaitForMultipleObjects` pattern. The shadow server already does this correctly; the simpler
tutorial examples don't.  
**PR value:** Medium. Reduces CPU usage for any lightweight server built on FreeRDP.

### 2. `rdpgfx_server_handle_messages` allocates a wStream per call

**File:** `channels/rdpgfx/server/rdpgfx_main.c`  
**Issue:** Every call to `rdpgfx_server_handle_messages` allocates a fresh `wStream` via
`Stream_New`. For a 60fps server this is 60 alloc/free pairs per second for the GFX channel
dispatch path alone, even when there are no messages pending.  
**Fix:** Pre-allocate a reusable stream in `RdpgfxServerPrivate` and reset it per call
(`Stream_SetPosition(s, 0)`). This eliminates 60 alloc/free pairs/second per connected client.  
**PR value:** High. Zero-allocation hot path for the most latency-sensitive channel.

### 3. `server_rdpsnd_get_formats` copies the format array every call

**File:** `server/server-common/server.c`  
**Issue:** Returns a freshly `malloc`d array on every call. Callers are expected to free it.
For our use case (called once per session) this is minor, but for implementations that
re-negotiate formats it's wasteful.  
**Fix:** Return a `const AUDIO_FORMAT *` pointing to a static array + a `size_t count`.
Callers that need a mutable copy can memcpy; callers that don't (like us) avoid the alloc.  
**PR value:** Low-medium. Clean API improvement, minor perf win.

### 4. RDPGFX `SurfaceCommand` re-serialises the PDU header on every frame

**File:** `channels/rdpgfx/server/rdpgfx_main.c`  
**Issue:** `rdpgfx_server_send_surface_command` writes the full RDPGFX_CMDID_WIRETOSURFACE_1
header bytes on every call, including re-encoding the surface ID and codec ID that don't
change between frames.  
**Fix:** Cache the constant header bytes in `RdpgfxServerPrivate` after the first frame and
use `memcpy` for the invariant prefix. Only the length field and data pointer change per frame.  
**PR value:** Medium. Shaves ~200ns of serialization per frame at 60fps → 12µs/second.

### 5. Static build: missing cmake preset for minimal server

**File:** `CMakePresets.json` (doesn't exist in 3.10.3)  
**Issue:** Building a minimal static server library requires passing 25+ `-DWITH_*=OFF` flags
with no documentation. We discovered the correct set through trial and error across 4 CI runs.  
**Fix:** Add a `server-minimal` cmake preset that encodes the correct flags for a server-only
static build. This benefits anyone building embedded/lightweight RDP servers.  
**PR value:** High documentation/DX value, zero code change.

---

## macOS framework opportunities

### `CGDisplayStream` — partial update encoding

We currently send full H.264 frames. `CGDisplayStreamUpdateRef` carries per-frame dirty rects.
The GFX pipeline supports per-region surface commands (`RDPGFX_SURFACE_COMMAND` with non-zero
`left/top/right/bottom`). Encoding only dirty tiles with VideoToolbox using
`kVTCompressionPropertyKey_SourceFrameCount = 0` (intra-only mode for tiles) and sending
multiple surface commands per frame would dramatically reduce bandwidth on static screens.

**Estimated win:** On a typical developer desktop (~20% of pixels changing per frame),
this reduces encoded bytes by ~4x.

### `AudioHardwareTapCreate` (macOS 14.2+, private → will be public)

Our current audio tap attaches an IO proc to the output device. This adds a processing stage
to the output chain and requires root. macOS 14.2 introduced `AudioHardwareTapCreate` (still
private but with public headers appearing in macOS 15 SDK) which creates a non-destructive tap
with lower latency and no root requirement. Tracking: rdar://120473958.

### `CGVirtualDisplayMode` refresh rate

We hardcode 60Hz. The client's `DesktopPhysicalWidth`/`DesktopPhysicalHeight` and monitor
layout PDU carry DPI and refresh information. Matching the client's native refresh rate
reduces tearing artifacts on the virtual display.

---

## License summary for upstream PRs

| Project | License | CLA required | PR target |
|---------|---------|--------------|-----------|
| FreeRDP | Apache 2.0 | No CLA | github.com/FreeRDP/FreeRDP |
| WinPR (part of FreeRDP) | Apache 2.0 | No CLA | Same repo |

All identified FreeRDP fixes are compatible with a clean Apache 2.0 contribution.
The cmake preset addition (#5 above) is the highest-value/lowest-risk PR to open first.

---

## Priority order for next session

1. Wire dirty rects into partial GFX surface commands (biggest bandwidth win)
2. Open FreeRDP PR: `wStream` pre-allocation in rdpgfx hot path
3. Open FreeRDP PR: cmake `server-minimal` preset  
4. Replace NSPasteboard polling with distributed notification
5. Gate audio capture on client capability flag
6. Adaptive frame rate based on client's declared refresh rate
