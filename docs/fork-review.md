# Forked-library review: FreeRDP & OpenSSL

Review of the two source-pinned forks the release binary is built from, scoped
to **what was changed versus upstream** (the only meaningful review surface).

- `grioghar/FreeRDP @ build/3.10.3-patched` — base tag upstream `3.10.3`
- `grioghar/openssl @ openssl-3.6.2`

Pins live in `.github/freerdp-version` (`3.10.3`) and `.github/openssl-version`
(`openssl-3.6.2`); both forks are cloned and built by `.github/workflows/{ci,release}.yml`.

## Summary

| Fork | Delta vs upstream | Verdict |
|------|-------------------|---------|
| OpenSSL | **None** — fork tree is byte-identical to upstream `openssl-3.6.2` | Clean mirror, nothing to review. See process note below. |
| FreeRDP | **One commit**: `perf(rdpgfx-server): pre-allocate send buffer` (~35 lines in `channels/rdpgfx/server/rdpgfx_main.{c,h}`) | Optimization is worthwhile; it introduces a latent cross-thread data race. |

The `grioghar/FreeRDP` repo also has a `perf/rdpgfx-preallocate-send-stream`
branch, but it is **not** part of the build (3527 commits off `master`, not
merged into `build/3.10.3-patched`). Only the single patch above ships.

## OpenSSL

The fork's `openssl-3.6.2` tree hashes identically to upstream
`openssl/openssl @ openssl-3.6.2`. There are no source patches — it is a pinned
mirror for reproducible builds. Nothing to review in the code.

**Process note (low):** as a manual mirror, this fork does not automatically
receive OpenSSL security releases. Because this is a TLS-terminating server,
add a reminder (or a small CI check that compares the pinned tag against the
latest upstream `openssl-3.x` tag) so a CVE fix is not missed.

## FreeRDP patch — what it does

`rdpgfx_server_packet_send` previously did a `Stream_New` / `Stream_Free` on
every outgoing GFX packet. The patch replaces that with a single 1 MiB
`send_stream` allocated once in `rdpgfx_server_context_new`, reused per call via
`Stream_SetPosition(fs, 0)` + `Stream_EnsureCapacity`, and freed in
`rdpgfx_server_context_free`.

**The optimization is valid and the bookkeeping is correct:** it removes one
malloc+free per packet (~60/s at 60 fps); there is no leak and no double-free
(the buffer is owned by the context, no longer freed on the send path); the
output length is taken from the write position, so no stale bytes from a prior
larger frame leak onto the wire.

## Finding 1 — Data race on the shared `send_stream` (correctness)

The patch comment states the buffer is *"Accessed only from the application
(send) thread."* That is **not true**. `rdpgfx_server_packet_send` is reached
from two different threads in this daemon:

1. **VideoToolbox encoder callback thread** — the H.264 frame path:
   `vt_callback` → `FrameEncoder.outputHandler` → `rdp_peer_send_h264_frame`
   (`protocol/RDPPeer.c`) → `gfx->SurfaceCommand` → `rdpgfx_server_packet_send`.
2. **Session-queue thread** (`com.macosrdp.session`) — the channel pump:
   `RDPSession setupAndRun` → `rdp_peer_run_once` → `rdpgfx_server_handle_messages`
   → `recv_caps_advertise_pdu` → `gfx_caps_advertise` →
   `gfx->CapsConfirm` / `CreateSurface` / `MapSurfaceToOutput` →
   `rdpgfx_server_packet_send`.

Upstream was implicitly safe here because each call owned a fresh `Stream_New`.
The patch shares one buffer across both threads with **no lock**. If two sends
overlap, both run `Stream_SetPosition(fs, 0)` then `zgfx_compress_to_stream(fs, …)`
on the same buffer:

- interleaved writes → corrupted ZGFX bytes on the wire → client graphics desync;
- worst case, a concurrent `Stream_EnsureCapacity` reallocs the buffer under the
  other thread's live pointer → use-after-free / **heap corruption / crash**.

### Why it is currently *mostly* masked (but still a real bug)

`gfx_caps_advertise` sets `ctx->gfxReady = true` only **after** its three setup
sends complete, and `rdp_peer_send_h264_frame` returns early while
`!gfxReady`. So on the normal, one-shot `CAPS_ADVERTISE` flow, the setup sends
finish before any frame is sent, and the two threads do not overlap. The daemon
also leaves the other GFX recv-callbacks (`CacheImportOffer`,
`FrameAcknowledge`, `QoeFrameAcknowledge`) `NULL`, so no other recv-thread send
exists today.

That safety is incidental and fragile. The race goes live if **any** of these hold:

- a client, proxy, or RD Gateway re-advertises GFX caps mid-session (a second
  `CAPS_ADVERTISE` runs `CapsConfirm`/`CreateSurface`/`MapSurfaceToOutput` on the
  session thread while the VT thread is streaming frames);
- a sending recv-callback is wired up later — e.g. `CacheImportReply`, which
  [MS-RDPEGFX] expects in response to a client `CACHE_IMPORT_OFFER`;
- `ctx->gfxReady` is observed across threads as a plain `bool` with no
  acquire/release barrier (see Finding 3), so the VT thread's view of "ready"
  and of the setup sends' memory effects is not guaranteed ordered.

Given this is exactly the connection-setup window where the project has been
fighting intermittent black-screen / `0x904`-class failures, the cheap fix is
worth taking even though the common path is gated.

### Recommended fix (smallest safe change)

Serialize the send body with a lock. Uncontended cost is negligible and it makes
the optimization correct regardless of caller threading and future callbacks.

```c
/* rdpgfx_main.h — in struct s_rdpgfx_server_private */
wStream*          send_stream;
CRITICAL_SECTION  send_lock;   /* guards send_stream */

/* rdpgfx_server_context_new, after send_stream is allocated */
InitializeCriticalSection(&priv->send_lock);

/* rdpgfx_server_context_free */
DeleteCriticalSection(&context->priv->send_lock);

/* rdpgfx_server_packet_send: hold the lock from Stream_SetPosition(fs, 0)
 * through WTSVirtualChannelWrite, released before Stream_Free(s) at out: */
EnterCriticalSection(&context->priv->send_lock);
...
LeaveCriticalSection(&context->priv->send_lock);
```

Alternatives: a small 2–3 buffer pool, or a thread-local send buffer. A lock is
simplest, and `WTSVirtualChannelWrite` ordering arguably wants serialization
anyway. The patch comment should also be corrected — the buffer is reached from
both the encoder callback thread and the session-queue thread.

## Finding 2 — Send buffer never shrinks (efficiency, low)

A single oversized frame grows `send_stream` via `Stream_EnsureCapacity`, and it
stays at that high-water mark for the connection lifetime. Not a leak, and an
acceptable trade-off — noted only so the per-session steady-state footprint
(largest I-frame, not 1 MiB) is understood, e.g. at 4K.

## Finding 3 — `gfxReady` cross-thread visibility (correctness, low, daemon-side)

`ctx->gfxReady` is written on the session-queue thread (`gfx_caps_advertise`,
`RDPPeer.c:189`) and read on the VT callback thread
(`rdp_peer_send_h264_frame`, `RDPPeer.c:482`) as a plain `bool` with no
atomic/barrier. This is what gates Finding 1's masking, so it is worth making
explicit: use `atomic_bool` (acquire/release) or publish readiness through the
session queue. Lives in this repo, not the fork.

## Bottom line

- **OpenSSL fork:** clean; only add an upstream-CVE watch.
- **FreeRDP fork:** keep the optimization, but add a lock around the reused
  `send_stream` (Finding 1) and fix the misleading comment. Optionally harden
  `gfxReady` (Finding 3). Finding 2 is informational.
