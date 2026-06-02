# FreeRDP fork patches

Security/correctness backports for the FreeRDP fork
(`grioghar/FreeRDP @ build/3.10.3-patched`) that the release binary is built
from. These are **not** applied to this repo's source — they patch the separate
FreeRDP fork. See `docs/freerdp-server-audit.md` for the full analysis.

## Why these live here

This app drives FreeRDP as a server with **NLA disabled**, so the cliprdr and
rdpsnd server channels parse input from **unauthenticated** clients. The audited
files are byte-identical to upstream FreeRDP 3.10.3, so these are inherited
upstream bugs.

## Applying

From a checkout of the FreeRDP fork on `build/3.10.3-patched`:

```bash
git am < 0001-cliprdr-server-fix-capability-overflow-and-bound-dataLen.patch   # or: git apply
git apply 0002-rdpsnd-server-fix-off-by-one-and-format-validation.patch
```

Both have been verified to apply cleanly against `build/3.10.3-patched`.

## Patches

### 0001 — cliprdr server: heap overflow + dataLen DoS (Findings A, B)

- **A (high):** `cliprdr_server_receive_capabilities` sized the `capabilitySets`
  buffer by the summed client `capabilitySetLength` bytes but wrote it as an
  array of typed structs. A `CB_CAPSTYPE_GENERAL` set with
  `capabilitySetLength < 12` undersized the allocation while the general handler
  wrote 12 bytes → up to **8 attacker-controlled bytes past the heap buffer**.
  Fix sizes by element count and validates the general-set length.
- **B (medium/DoS):** `cliprdr_server_read` fed an untrusted 32-bit `dataLen`
  straight into `Stream_EnsureCapacity` (multi-GB allocation) with a 32-bit wrap
  on `+ CLIPRDR_HEADER_LENGTH`. Fix clamps to 64 MiB and uses `size_t` math.

**Upstream status:** both are **already fixed in FreeRDP master** (A via the same
length validation; B via a refactor). Preferred long-term fix is to **bump the
FreeRDP pin**; this patch is the minimal backport if staying on 3.10.3.

### 0002 — rdpsnd server: off-by-one + dead format check (Findings C, D)

- **C (low):** `rdpsnd_server_send_wave_pdu` used `>` where the three sibling
  send paths use `>=`, allowing a one-element out-of-bounds read of
  `client_formats[]`.
- **D (low):** the post-loop `if (!num_client_formats)` guard in
  `rdpsnd_server_recv_formats` is dead code; it was meant to test
  `num_known_format`, so a client offering only `wFormatTag==0` formats passed
  negotiation with zero usable formats.

**Upstream status:** both are **still present in FreeRDP master (3.26)** — a
bump does *not* fix these, so the patch is needed regardless. Worth reporting
upstream.

> Not addressed here: the `rdpsnd` re-negotiation leak (a second `SNDC_FORMATS`
> leaks the prior `client_formats`). A correct fix must capture the old format
> count before it is overwritten and free with the proper `audio_formats_free`;
> left as a follow-up because it needs restructuring, and re-negotiation is rare.
