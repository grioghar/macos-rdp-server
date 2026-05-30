---
name: macos-rdp-server-state
description: Current build/release state, all known issues, efficiency roadmap for macos-rdp-server
metadata:
  type: project
---

## Repository
github.com/grioghar/macos-rdp-server — public, MIT license.

## Current state (as of v0.1.2 attempts)
- v0.1.1 released but CI for v0.1.2 has been iterating through FreeRDP build issues.
- Latest CI run: 26668733140 (triggered after the API fix commit 8c1b578). Status: in-progress.
- All known compile errors have been fixed in commit 8c1b578.

## What was fixed in the last commit (8c1b578)
Every error was confirmed against actual FreeRDP 3.10.3 headers before being fixed:
- `peer->ContextSize` (not `context_size`)
- `peer->context->input` (not `peer->input`)  
- VCM: `WTSOpenServerA((LPSTR)peer->context)` / `WTSCloseServer` — it does NOT live on rdpContext
- `FreeRDP_TlsSecLevel` (not `TLSSecLevel`); removed nonexistent `FreeRDP_SoundSystem`
- `RDPGFX_CAPS_CONFIRM_PDU.capsSet` is a pointer — stack-alloc `RDPGFX_CAPSET`, point to it
- `RDPGFX_CAPVERSION_7` does not exist — lowest is `RDPGFX_CAPVERSION_8`
- CLIPRDR structs use `.common.msgFlags` / `.common.dataLen` (CLIPRDR_HEADER is first field)
- `rdpsnd`: `server_rdpsnd_get_formats` + `Activated` callback + `SelectFormat` pattern (from shadow_rdpsnd.c)
- `AudioRedirect`: real `SendSamples(ctx, buf, nframes, ts)` signature — no RDPSND_DATA_BLOCK struct
- `display/VirtualDisplay.m`: `#import <CoreGraphics/CoreGraphics.h>` not `CGVirtualDisplay.h`
- Peer loop: replaced polling with `WaitForMultipleObjects` over GetEventHandles + VCM + GFX events

## FreeRDP build flags (confirmed working after 4 CI failures)
Key additions beyond the obvious: `-DWITH_PROXY=OFF`, `-DWITH_SWSCALE=OFF`, `-DWITH_DSP_FFMPEG=OFF`,
`-DWITH_CAIRO=OFF`, `-DWITH_OPUS=OFF`, `-DWITH_RDTK=OFF`, `-DWITH_WINPR_TOOLS=OFF`, `-DBUILD_TESTING=OFF`.
Cache key: `freerdp-3.10.3-{arch}-static-v3`.

## Efficiency roadmap (full analysis in docs/efficiency-analysis.md)

### In our code (priority order)
1. Wire `CGDisplayStreamUpdateRef` dirty rects → partial GFX surface commands (biggest win)
2. Replace NSPasteboard 500ms poll with `NSDistributedNotificationCenter` clipboard change notification
3. Gate audio capture on `FreeRDP_AudioPlayback` client capability
4. Adaptive frame rate from client's declared `DesiredFrameRate`
5. Replace main run-loop `runUntilDate:1s` with `kqueue` EVFILT_SIGNAL

### FreeRDP upstream PRs (Apache 2.0, no CLA)
1. **cmake `server-minimal` preset** — high DX value, zero code risk, open first
2. **Pre-allocate wStream in rdpgfx hot path** — eliminates 60 alloc/free pairs/second per client
3. **`server_rdpsnd_get_formats` static array** — clean API, avoids malloc-per-call

## User preferences
- "As lightweight as possible while still being quality software"
- Monitor CI, fix failures immediately from actual error output (not guessing)
- Read upstream source headers before writing any code that calls them
- Leave handoff notes before tokens run out

## How to continue next session
1. Check if CI run 26668733140 succeeded: `gh run view 26668733140 --repo grioghar/macos-rdp-server`
2. If it failed, get logs: `gh api "repos/grioghar/macos-rdp-server/actions/jobs/{job_id}/logs"`
3. If it succeeded, implement dirty-rect partial updates (item 1 above) — see docs/efficiency-analysis.md
4. Then open the FreeRDP cmake preset PR
