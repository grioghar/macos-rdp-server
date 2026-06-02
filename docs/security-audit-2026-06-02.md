# Security & Optimization Audit — 2026-06-02

Standing mandate: **scan every forked library for security + optimization findings,
keep the daemon lightweight, contribute fixes upstream (Apache 2.0).** This is run
continuously, not once. Forks in use: `grioghar/FreeRDP` (build/3.10.3-patched),
`grioghar/fuse` (RETIRED — see below), OpenSSL (static, mirror).

## A. FreeRDP fork (grioghar/FreeRDP @ build/3.10.3-patched)

### HIGH — fixed in our fork (client-triggerable)
1. **rdpsnd divide-by-zero (SIGFPE)** — `channels/rdpsnd/server/rdpsnd_main.c`,
   `rdpsnd_server_recv_formats()`. Client format with `nChannels==0`/`nBlockAlign==0`
   used as a divisor → server crash. Upstream master already fixed; applied the same
   check to our build branch. (No upstream PR needed — upstream has it.)
2. **rdpgfx silent corrupt-frame send** — `channels/rdpgfx/server/rdpgfx_main.c`,
   `rdpgfx_send_surface_frame_command()`. Three bare `goto error;` left `error`
   == CHANNEL_RC_OK, returning success on a truncated PDU. Added
   `error = ERROR_INTERNAL_ERROR;` before each (matches upstream). No upstream PR needed.

### MEDIUM — not fixed upstream either → FILE UPSTREAM PRs
3. **cliprdr unbounded dataLen allocation DoS** — `channels/cliprdr/server/cliprdr_main.c`
   (~line 817). `header.dataLen` (UINT32, wire) → `Stream_EnsureCapacity` with no cap;
   client sets ~4 GB → per-connection memory-exhaustion DoS. Fix: reject dataLen > 64 MiB.
4. **cliprdr format-list numFormats integer overflow** — `channels/cliprdr/cliprdr_common.c`
   (~line 280). `numFormats * 36` (UINT32) overflows → tiny initial alloc → heavy realloc
   churn. Fix: `if (numFormats > UINT32_MAX/36) return NULL;`

### LOW — documented, not urgent
5. rdpdr IRP CompletionId key truncation on 32-bit (`rdpdr_main.c:191`) — 64-bit safe.
6. rdpgfx capsSet->length uncapped seek (`rdpgfx_main.c:~1325`) — handled gracefully, no log.

### PERF — ready to file upstream
7. **perf/rdpgfx-preallocate-send-stream** — pre-allocates the ZGFX send buffer (1 MiB)
   instead of malloc/free per packet (~60/s/client at 60fps). Already merged into our
   build branch. NOT in upstream. READY TO FILE.

### Fork branch status
- `perf/rdpgfx-preallocate-send-stream`: in build branch; FILE upstream (cmd below).
- `add-server-minimal-preset`: already filed upstream as FreeRDP PR #12826.
- `fix/rdpsnd-server-format-divzero`, `fix/rdpgfx-server-frame-cmd-error-code`: placeholder
  branches (empty vs upstream master, which already has the fixes). The actual fixes were
  applied directly to build/3.10.3-patched (items 1-2 above).

## B. FUSE fork (grioghar/fuse) — RETIRED
Forked osxfuse/fuse 2.9.9. **Dead end**: unconditionally requires the macFUSE KEXT at
runtime (hardcoded `/dev/macfuse*` + `mount_macfuse`); cannot run on a stock Mac without
admin kext approval + reboot, and a private `<strhash.h>` header blocks a clean source build.
3 minor security findings (system() in mount_run, unchecked argv[32]/calloc) noted for the
upstream osxfuse tracker, but **we will not use it.**

**RDPDR drive mounting → Apple File Provider framework** (NSFileProviderReplicatedExtension,
macOS 12+): no kext, no external dep, first-party, sandboxable, designed for remote-drive-
as-local-Finder-location (what iCloud Drive/Dropbox use). This is the path forward for actual
mounting; the RDPDR protocol layer uses FreeRDP's own rdpdr server API (WITH_RDPDR=ON).

## C. Our daemon code (macos-rdp-server) — PR #18 (fix/security-audit)
- RDPServer.m: active-session flag `volatile`→`_Atomic` (acquire/release) — prevents
  auto-update applying mid-session (binary corruption).
- RDPPeer.c: clipData/clipLen/clipFormat TOCTOU — malloc+copy outside lock, brief locked swap.
- AutoUpdate.m: SHA-256 regex matched an embedded 64-hex substring → integrity-check bypass;
  anchored with lookarounds.
- RDPLog.c: 1024→4096 body buffer (silent truncation of diagnostics).
- AudioCapture.m: per-callback (~100Hz) malloc/free → pre-allocated `_pcmBuf` reused.
- Deferred (need hardware test): Authenticator dscl-fallback password in argv (ps-visible),
  FreeRDP credential-string lifetime, VirtualDisplay usleep(400ms) on the session queue.

## Upstream PR commands (run when authed to FreeRDP/FreeRDP)
```bash
# PERF: pre-allocated rdpgfx send buffer
gh pr create -R FreeRDP/FreeRDP \
  --title "perf(rdpgfx-server): pre-allocate send buffer to eliminate per-packet malloc" \
  --head grioghar:perf/rdpgfx-preallocate-send-stream \
  --body "Reuse a 1 MiB send_stream in rdpgfx_server_packet_send instead of malloc/free per packet (~60/s/client at 60fps, 200-500KB each). Send functions are single-threaded from the app side; no extra sync. Freed in rdpgfx_server_context_free."

# MEDIUM: cliprdr dataLen cap + numFormats overflow guard — needs a small branch first:
#   (branch off grioghar/FreeRDP build/3.10.3-patched, add the two guards, push, then:)
#   gh pr create -R FreeRDP/FreeRDP --title "fix(cliprdr): bound dataLen + numFormats to prevent alloc DoS/overflow" --head grioghar:fix/cliprdr-bounds ...
```
