# FreeRDP server-side audit (inherited upstream code)

A review of the FreeRDP **library** code this app actually drives in its role as
a 60 fps macOS screen-streaming RDP server — looking for bugs, inefficiency, and
better ways to do the job. This complements `docs/fork-review.md` (which covers
the fork's one custom patch).

## Important framing

The fork (`grioghar/FreeRDP @ build/3.10.3-patched`) changes **only**
`channels/rdpgfx/server/rdpgfx_main.{c,h}`. Every other file below is
**byte-identical to upstream FreeRDP 3.10.3** (verified with `git diff 3.10.3`).
So the findings here are **upstream bugs the app inherits**, not fork
regressions. They still matter because:

- the app enables the **cliprdr** and **rdpsnd** server channels for every client, and
- the app **disables NLA** (see `fix(security): disable NLA`), so these channels
  parse PDUs from **unauthenticated** clients that can reach port 3389.

Server-side channel code in FreeRDP is far less exercised than the client paths,
so latent bugs survive here.

Modules audited (all on the app's actual data paths):
`libfreerdp/codec/zgfx.c`, `channels/cliprdr/server/cliprdr_main.c`,
`channels/rdpsnd/server/rdpsnd_main.c`, plus the rdpgfx server path.

---

## 🔴 Finding A — Heap overflow in cliprdr capability-set parsing (high; potentially critical)

`cliprdr_server_receive_capabilities` (`cliprdr_main.c:508`) sizes the
destination buffer by the **sum of client-supplied `capabilitySetLength`
bytes**, but indexes and writes it as an **array of typed structs**:

```c
cap_sets_size += capabilitySetLength;                 // 536: client-controlled byte count
if (cap_sets_size > 0)
    tmp = realloc(capabilities.capabilitySets, cap_sets_size);   // 539: sized in bytes
capabilities.capabilitySets = (CLIPRDR_CAPABILITY_SET*)tmp;
capSet = &(capabilities.capabilitySets[index]);       // 549: indexed per element
... cliprdr_server_receive_general_capability(context, s, (CLIPRDR_GENERAL_CAPABILITY_SET*)capSet);
```

`cliprdr_server_receive_general_capability` (`:469`) then writes `version` (off
4) and `generalFlags` (off 8) — **12 bytes total** into `capSet`. The stream
check at `:475` validates only that the *input* has 8 bytes; nothing validates
`capabilitySetLength` or the destination size.

**Exploit:** a client sends one `CB_CAPSTYPE_GENERAL` set with
`capabilitySetLength` of 4–11. The buffer is realloc'd to that many bytes, but
the general-capability handler writes 12 bytes → **up to 8 attacker-controlled
bytes (`version`, `generalFlags`) written past the heap allocation.** At minimum
a crash (DoS); potentially exploitable depending on heap layout. Reachable
pre-auth because NLA is disabled.

The legitimate case works only by coincidence: a well-formed general set has
`capabilitySetLength == 12 == sizeof(CLIPRDR_GENERAL_CAPABILITY_SET)`, so the
byte-sized buffer happens to fit the element write. Multiple sets also produce
overlapping writes (element stride is `sizeof(CLIPRDR_CAPABILITY_SET)` = 4, but
each general write is 12 bytes).

**Fix:** size the allocation by element count, validate `capabilitySetLength`:
```c
if (capabilitySetType == CB_CAPSTYPE_GENERAL && capabilitySetLength < CB_CAPSTYPE_GENERAL_LEN)
    goto out;
cap_sets_size = (index + 1) * sizeof(CLIPRDR_GENERAL_CAPABILITY_SET);
tmp = realloc(capabilities.capabilitySets, cap_sets_size);
```

---

## 🟠 Finding B — Untrusted `dataLen` drives allocation in cliprdr read loop (medium; DoS)

`cliprdr_server_read` (`cliprdr_main.c:1108`) reads a 32-bit `header.dataLen`
straight from the client and uses it to size the stream:

```c
Stream_Read_UINT32(s, header.dataLen);                 // 1114
if (!Stream_EnsureCapacity(s, (header.dataLen + CLIPRDR_HEADER_LENGTH)))   // 1116
```

`dataLen` is unbounded, so a client can request a ~4 GB allocation
(memory-exhaustion DoS). Separately, `header.dataLen + CLIPRDR_HEADER_LENGTH` is
computed in 32-bit unsigned and **wraps** when `dataLen` is near `UINT32_MAX`
(the constant is 8), so the subsequent length comparisons at `:1124`/`:1151`
operate on a wrapped value. The wrap path most likely lands on a safe parse
failure (sub-parsers use `Stream_CheckAndLogRequiredLength`), so the practical
risk is the large-allocation DoS. **Fix:** clamp `dataLen` to a sane maximum
(clipboard PDUs are small) and do the `+ CLIPRDR_HEADER_LENGTH` math in `size_t`
with an explicit overflow check.

---

## 🟡 Finding C — rdpsnd off-by-one bounds check (low; OOB read)

`rdpsnd_server_send_wave_pdu` (`rdpsnd_main.c:509`) guards with `>`, while the
three sibling send paths (`:680`, `:708`, `:815`) all use `>=`:

```c
if (context->selected_client_format > context->num_client_formats)   // 509  (should be >=)
    return ERROR_INTERNAL_ERROR;
...
format = &context->client_formats[context->selected_client_format];  // OOB when == num
```

When `selected_client_format == num_client_formats`, the check passes and the
indexing reads one element past `client_formats`. **Fix:** change `>` to `>=`
to match the siblings.

## 🟡 Finding D — rdpsnd dead validation / re-negotiation leak (low)

`rdpsnd_server_recv_formats` (`rdpsnd_main.c`):
- `:259` `if (!context->num_client_formats)` is **dead** — already guaranteed
  non-zero at `:208` and never changed in the loop. The intent was the
  `num_known_format` counter computed at `:255`; as written, a client
  advertising only `wFormatTag == 0` formats passes negotiation with zero usable
  formats. **Fix:** `if (!num_known_format) goto out_free;`.
- `:214` assigns `context->client_formats` without freeing a previous array, so
  a client that re-sends `SNDC_FORMATS` leaks the prior allocation; `out_free`
  (`:267`) frees without nulling, risking a later double-free via
  `context_free`. **Fix:** free+NULL before reallocating and on the error path.

---

## Efficiency / "better way" notes

### E1 — The server ZGFX "compressor" is a no-op passthrough (informational, and correct here)

`zgfx_compress_segment` (`zgfx.c:495`) is literally `/* FIXME: compression not
implemented. Just copy the raw source */` — it writes the 1-byte header then
`Stream_Write(s, pSrcData, SrcSize)` with the uncompressed flag. So every GFX
packet goes out ZGFX-**framed but not compressed**.

For this app that is the *right* behavior — the payload is already H.264, which
must not be re-compressed — so there's no efficiency win to chase in ZGFX, and
the fork's `send_stream` patch is feeding a passthrough. Two honest implications:
- the patch's comment calling this "ZGFX compressed output" is misleading; it's
  ZGFX-framed.
- the only real cost on the hot path is the single `Stream_Write` copy of the
  H.264 frame into the framing buffer (`zgfx.c:508`). Removing even that would
  require scatter-gather channel writes (header iovec + payload iovec); marginal,
  not worth it.

The ZGFX *decompress* path (`zgfx_decompress`, `:411`) is client→server bulk
data and is **not used** by this server's video path — dead weight for this app.

### E2 — rdpsnd does two full copies + imposes a latency floor for PCM-only audio (efficiency)

The app advertises **PCM only**, but the audio path still: copies CoreAudio
frames into `priv->out_buffer`, accumulates up to `latency` ms (default 50 ms),
then `freerdp_dsp_encode` copies the identical PCM bytes again into the PDU
stream. For PCM that DSP "encode" is a memcpy. Net: **two copies per chunk plus
up to 50 ms of buffering latency** that exists mainly to amortize ADPCM block
alignment — irrelevant when only PCM is negotiated. **Better:** when the source
format equals the selected (PCM) client format, frame each `SendSamples` call as
one Wave2 PDU and `Stream_Write` directly, skipping the `out_buffer` stage and
the latency floor. (This is app-tunable via the rdpsnd `latency`/format setup in
`RDPPeer.c`, no fork change required.)

### E3 — `selected_client_format` data race (low, daemon-side mitigation)

`rdpsnd_server_close` writes `selected_client_format = 0xFFFF` without holding
`priv->lock`, while `SendSamples` reads it under the lock from the CoreAudio
thread. Aligned-word so benign in practice, but a real race; the daemon should
ensure `Close` and `SendSamples` don't run concurrently (or the store should be
under the lock).

---

## Recommendations

1. **Findings A and B are the priority** — they're memory-safety / DoS issues in
   code that parses unauthenticated client input (NLA is off). Options, best first:
   - **Re-enable NLA** (or otherwise authenticate before channel data flows) so
     these parsers aren't exposed pre-auth — addresses the exposure broadly.
   - **Upgrade the pinned FreeRDP** if a newer release fixes these, and/or carry
     targeted fork patches for A/B. (These should also be reported upstream.)
2. **Findings C and D** are cheap one-line fork patches; low urgency.
3. **E2** is a worthwhile latency + CPU win for audio and needs no fork change.
4. **E1** confirms there's no ZGFX compression win to chase — leave it.

> Not yet verified: whether A–D are already fixed in FreeRDP `master`/3.11+. That
> determines whether the fix is "bump the pin" or "carry a patch." Worth checking
> before patching the fork.
