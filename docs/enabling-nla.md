# Re-enabling authentication (NLA) — root cause and plan

This is the P0 from `docs/freerdp-server-audit.md`: there is **no authentication**
before the cliprdr/rdpsnd/gfx parsers and the input injector run, because NLA was
disabled. This documents *why*, the build fix, and the exact app-side changes.

> **Scope note.** The authenticated path is now **implemented but gated behind
> the `MACOS_RDP_ENABLE_NLA` CMake option (default OFF)**. With the option OFF —
> the default build, CI, and the current installer — the security configuration
> is byte-for-behavior identical to before (TLS + legacy RDP, no auth), so there
> is no connectivity regression risk. Turning the option ON compiles the NLA
> path and requires an OpenSSL that exposes the legacy provider. The OpenSSL
> build changes and the NLA handshake itself **cannot be compiled or tested from
> the Linux CI container these notes were written in**, so validate the
> `-DMACOS_RDP_ENABLE_NLA` build on real macOS against `mstsc` before relying on
> it. The always-on safe change is honoring a failed certificate load
> (`peer_apply_settings` now returns `bool`; `rdp_peer_create` aborts).

## What's now in the tree

| File | Role |
|---|---|
| `protocol/RDPSecurity.{h,c}` | `rdp_security_init_crypto()` (load legacy provider + probe MD4), `rdp_security_load_credentials()` |
| `protocol/RDPPeer.c` | security block gated on `MACOS_RDP_ENABLE_NLA` + credentials; one-time crypto init; honors cert-load failure |
| `scripts/set-rdp-credentials.sh` | provisions `/etc/macos-rdp/credentials` storing only the NTLM hash (self-contained MD4, verified against standard vectors) |
| `CMakeLists.txt` | `MACOS_RDP_ENABLE_NLA` option; conditional OpenSSL include/link + legacy archives |

### Turning it on

```bash
# 1. Build OpenSSL so liblegacy.a / libcommon.a are available (see "Fix part 1").
# 2. Configure with NLA:
cmake -B build -DMACOS_RDP_ENABLE_NLA=ON [-DMACOS_RDP_STATIC=ON -DFREERDP_STATIC_INSTALL=... -DOPENSSL_ROOT_DIR=...]
cmake --build build
# 3. Provision a credential (stores only the NTLM hash):
sudo ./scripts/set-rdp-credentials.sh myuser
# 4. Restart the daemon; it logs "NLA enabled — authenticating user 'myuser'".
```

If `MACOS_RDP_ENABLE_NLA=ON` but no credential is provisioned (or MD4 is
unavailable), the daemon logs a loud `SECURITY: ... UNAUTHENTICATED` warning and
falls back to the current TLS-only behavior rather than refusing all
connections — change this to fail-closed if you prefer.

## Why NLA was disabled (root cause)

`protocol/RDPPeer.c:73-76` says it outright: NLA needs NTLM, NTLM needs MD4, and
*"our minimal OpenSSL build omits"* it — the runtime log line is
`md4 NTLM support not available`.

The deeper reason is **OpenSSL 3.x providers**. NTLM (CredSSP/NLA's auth
mechanism) requires three primitives that OpenSSL 3.x moved out of the default
provider into the **legacy provider**:

- **MD4** — the NTLM password hash,
- **RC4** — NTLMv2 key exchange / sealing,
- **DES** — LM/NTLMv1 response.

The release build configures OpenSSL with (`.github/workflows/release.yml`):

```
./Configure <target> no-shared no-tests no-docs no-apps --libdir=lib --prefix=...
make && make install_dev
```

`no-shared` is fine, and it does *not* pass `no-legacy`, so the legacy code is
compiled. But two things are missing:

1. `make install_dev` installs `libssl.a` + `libcrypto.a` + headers — it does
   **not** install/expose the legacy provider archive (`providers/liblegacy.a`),
   so the final binary never links it.
2. Even when linked, the legacy provider is **not active by default** — only the
   `default` provider auto-loads. MD4 via `EVP_MD_fetch(NULL, "MD4", NULL)` /
   WinPR's `winpr_Digest_New(WINPR_MD_MD4)` returns failure until the legacy
   provider is explicitly loaded.

So WinPR's NTLM module can't compute the MD4 hash → NTLM unavailable → NLA can't
be offered → the app fell back to TLS-only (plus legacy RDP) with no auth.

(For reference, the upstream FreeRDP sample server `sfreerdp.c:1121` also disables
NLA — server-side NLA is a known-awkward path, which is why this needs care.)

## Fix part 1 — make the legacy provider available and active (build + startup)

### 1a. Build: ship the legacy provider archive

In the OpenSSL build step, after `make`, also retain the static provider
archives so they can be linked. With `no-shared`, OpenSSL builds
`providers/liblegacy.a` and `providers/libcommon.a`. Copy them alongside
`libcrypto.a`:

```sh
# after `make install_dev` in build_arch()
cp "$src/providers/liblegacy.a"  "$prefix/lib/"
cp "$src/providers/libcommon.a"  "$prefix/lib/"   # legacy depends on libcommon
```

…then `lipo`-merge `liblegacy.a`/`libcommon.a` into the universal set just like
`libssl`/`libcrypto`, and add them to the final link line **after** `libcrypto`
(`CMakeLists.txt`, the `OPENSSL_STATIC_LIBS` glob already picks up `lib/*.a` for
the static path — confirm `liblegacy.a` lands there and is ordered after
`libcrypto.a`).

### 1b. Startup: register + load the legacy provider

For a fully static binary there is no `legacy.dylib` to dlopen, so register the
provider as a built-in and load it programmatically, once, before any NTLM use.
Do this next to the existing `winpr_InitializeSSL` call in `rdp_peer_create`
(`RDPPeer.c:357`), or better in daemon startup:

```c
#include <openssl/provider.h>

/* Entry point exported by providers/liblegacy.a (internal symbol). */
extern OSSL_provider_init_fn ossl_legacy_provider_init;

static void load_openssl_legacy_provider(void) {
    if (!OSSL_PROVIDER_add_builtin(NULL, "legacy", ossl_legacy_provider_init) ||
        !OSSL_PROVIDER_load(NULL, "legacy") ||
        !OSSL_PROVIDER_load(NULL, "default")) {
        rdp_error("failed to load OpenSSL legacy provider — NLA/NTLM unavailable");
        return;
    }
    /* Probe MD4 so we fail loudly rather than at handshake time. */
    EVP_MD *md4 = EVP_MD_fetch(NULL, "MD4", NULL);
    if (!md4) { rdp_error("MD4 still unavailable after loading legacy provider"); return; }
    EVP_MD_free(md4);
    rdp_info("OpenSSL legacy provider active (NTLM/MD4 available)");
}
```

> Note: `ossl_legacy_provider_init` is an OpenSSL-internal symbol. If the static
> link can't resolve it, the alternative is to ship an `openssl.cnf` that
> activates `[legacy_sect]` and point `OPENSSL_CONF` at it — but that breaks the
> "self-contained single binary" property, so the built-in registration above is
> preferred. **This is the part most likely to need iteration in CI.**

## Fix part 2 — turn on NLA and harden the security layer (app-side)

Server-side NLA validates the client's NTLM response against configured
credentials (`libfreerdp/core/nla.c:304-410`): it uses `FreeRDP_Username` plus
either `FreeRDP_Password` or a 32-hex-char `FreeRDP_PasswordHash` (NTLM hash), or
a SAM file. Provision a credential and store the **NTLM hash**, never plaintext.

### 2a. Credential source (root-only)

Add `/etc/macos-rdp/credentials` (mode `0600`, root-owned), e.g.:

```
username=rdp
# NTLM hash of the password: openssl/`samba`-style MD4(UTF-16LE(password))
passwordhash=8846f7eaee8fb117ad06bdd830b7586c
```

Generate the hash at install time (helper in `scripts/`), so the plaintext
password never lands on disk.

### 2b. `peer_apply_settings` diff (replace the security block, `RDPPeer.c:73-81`)

```c
/* Security: require TLS + NLA. Drop the legacy RDP security layer entirely so
 * a client cannot negotiate an unauthenticated / weakly-encrypted path. */
freerdp_settings_set_bool(s,   FreeRDP_UseRdpSecurityLayer, FALSE);
freerdp_settings_set_bool(s,   FreeRDP_RdpSecurity,         FALSE);   /* was TRUE */
freerdp_settings_set_bool(s,   FreeRDP_TlsSecurity,         TRUE);
freerdp_settings_set_uint32(s, FreeRDP_TlsSecLevel,         2);       /* was 1 */
freerdp_settings_set_uint32(s, FreeRDP_EncryptionLevel,     ENCRYPTION_LEVEL_HIGH);

struct rdp_credentials cred;
if (load_rdp_credentials(&cred)) {                 /* reads /etc/macos-rdp/credentials */
    freerdp_settings_set_bool(s,   FreeRDP_NlaSecurity,  TRUE);
    freerdp_settings_set_string(s, FreeRDP_Username,     cred.username);
    freerdp_settings_set_string(s, FreeRDP_PasswordHash, cred.password_hash);
    rdp_info("NLA enabled (user '%s')", cred.username);
} else {
    /* Fail closed if NLA was requested but no credential is provisioned. */
    freerdp_settings_set_bool(s, FreeRDP_NlaSecurity, FALSE);
    rdp_error("SECURITY: no credentials configured — NLA OFF, connections are "
              "UNAUTHENTICATED. Create /etc/macos-rdp/credentials to require auth.");
}
```

With NLA on, `peer_post_connect` (channel open) and the input callbacks only run
*after* CredSSP succeeds — so this single change gates the clipboard/audio/gfx
parsers **and** keyboard/mouse injection behind authentication. No separate
channel/input gating is needed once NLA is in force.

### 2c. Defense-in-depth (independent of NLA)

- **Bind to loopback / a private interface by default** (`RDPServer.m` uses
  `in6addr_any`; launchd plist `SockNodeName = ::`). Make the bind address a
  config option defaulting to loopback, so the daemon isn't internet-facing
  unless the operator opts in. This protects the parsers regardless of the auth
  fix and is the cheapest single mitigation.
- Consider not registering `KeyboardEvent`/`MouseEvent` (`RDPPeer.c:383-385`)
  until `peer_activate`; with NLA this is redundant, but it's belt-and-suspenders
  if NLA can't be enabled short-term.

## Fallback if the legacy provider can't be made to work

If wiring the static legacy provider proves too painful, the next-best postures,
in order:

1. **Bind to loopback + require an SSH/WireGuard/Tailscale tunnel** for remote
   access. No NLA, but the parsers are no longer exposed to the open network.
2. Keep TLS-only but **drop legacy `RdpSecurity`** and raise `TlsSecLevel` to 2
   (2b above, minus the NLA block) — still unauthenticated, but at least always
   encrypted with modern ciphers and no weak fallback.

Option 1 is strongly preferred over shipping an unauthenticated, internet-facing,
root RDP server.

## Verification plan (on a real macOS build)

1. Build with the legacy provider; on daemon start confirm the log line
   `OpenSSL legacy provider active (NTLM/MD4 available)` (and **not**
   `md4 NTLM support not available`).
2. With `/etc/macos-rdp/credentials` present, connect with `mstsc` /
   `xfreerdp /sec:nla` — verify the client is prompted for credentials and that
   wrong credentials are rejected *before* any desktop/channel activity.
3. Confirm `xfreerdp /sec:rdp` (legacy) is now **refused**.
4. Re-run the connectivity regression the project cares about (the 0x904 /
   black-screen path) to ensure NLA + TLS-only didn't reintroduce it.
5. Negative test: remove the credentials file → confirm the loud warning and that
   you've made a conscious choice (or, preferably, fail closed).
