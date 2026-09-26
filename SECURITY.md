# Security

This document describes the security model of `macos-rdp-daemon` as it is
actually implemented in this repository, and how to report a vulnerability.
Every claim below is drawn from the code and scripts in the tree (file paths
are given); anything that could not be confirmed from the code is marked with
a placeholder.

## Threat model

### What the daemon is

`macos-rdp-daemon` is an RDP server for macOS. It embeds a static build of
FreeRDP (fork `grioghar/FreeRDP`, branch `build/3.10.3-patched`; see
`.github/workflows/ci.yml`) for the RDP protocol itself and adds the macOS
side: a per-session `CGVirtualDisplay`, ScreenCaptureKit capture, VideoToolbox
H.264 encoding, keyboard/mouse injection, bidirectional clipboard, system-audio
capture, optional microphone redirection and optional client-drive
redirection.

Consequently there are two trust boundaries to keep in mind:

- **An authenticated RDP client is equivalent to the console user.** After
  authentication the client sees the logged-in user's desktop, drives the
  keyboard and mouse (`input/InputInjector.m`, `CGEventPost`), reads and writes
  the pasteboard (`input/ClipboardSync.m`), hears system audio
  (`audio/AudioCapture.m`) and, when the corresponding feature flags are set,
  can play sound into the Mac's speakers (`audio/AudioInput.m`) and expose its
  own drives to the Mac (`protocol/RDPPeer.c`, `protocol/RDPWebDAV.c`).
- **The daemon's process identity is either root or the logged-in user.** The
  system LaunchDaemon (`launchd/com.macosrdp.daemon.plist`, installed by
  `scripts/install.sh` / `scripts/remote-install.sh`) runs it as `root`. The
  per-user LaunchAgent (`launchd/com.macosrdp.agent.plist`, written by
  `scripts/install-user.sh`) runs it as the logged-in user in the Aqua session.
  Any bug reachable by a remote client therefore executes with one of those
  identities.

### Network exposure

- **Listening socket.** `daemon/RDPServer.m` creates an `AF_INET6` socket with
  `IPV6_V6ONLY=0` bound to `in6addr_any` on the configured port (default 3389,
  `--port`). It therefore listens on **every interface, IPv4 and IPv6**. There
  is no bind-address option, no allow-list, no rate limiting and no lockout
  after failed logins. The launchd plists additionally declare a launchd
  socket on `::`/3389, but the daemon does not consume it; it always opens its
  own socket.
- **Expected network.** The code assumes a private network: `daemon/main.m`
  holds a system-sleep assertion so the Mac "stays reachable on the LAN /
  Tailscale". Nothing in the daemon is designed for direct Internet exposure.
  Put it behind a firewall, VPN or Tailscale ACL and do not port-forward 3389.
  <!-- PLACEHOLDER(greg): confirm the intended deployment network (LAN only? Tailscale? anything else?) -->
- **Unauthenticated surface, in order of arrival.**
  1. *Pre-TLS.* The X.224 Connection Request and security negotiation are
     handled entirely by FreeRDP. `protocol/RDPPeer.c` (`peer_apply_settings`)
     enables TLS (`FreeRDP_TlsSecurity`, `FreeRDP_TlsSecLevel` 1) and
     **disables NLA/CredSSP** (`FreeRDP_NlaSecurity` FALSE, because the static
     OpenSSL build has no MD4/NTLM). It also leaves the legacy **Standard RDP
     Security** layer enabled (`FreeRDP_RdpSecurity` TRUE,
     `FreeRDP_UseRdpSecurityLayer` FALSE). Which layer is used is chosen during
     the negotiation from what the client requests; a client that requests only
     Standard RDP Security gets an RC4-based, non-TLS session. There is no
     server-side option to force TLS.
  2. *Inside the encrypted session but before authentication.* FreeRDP
     processes MCS, licensing, the Client Info PDU (which carries the username,
     password and domain in the clear inside the negotiated encryption) and the
     capability exchange. The daemon's `peer_post_connect` then creates the
     virtual-channel manager and opens **cliprdr, rdpsnd, rdpgfx/drdynvc**, and
     (when `RDP_RDPDR_ENABLED=1`) **rdpdr**, and (when `RDP_AUDIO_INPUT=1`)
     **AUDIO_INPUT**, all before any credential is checked.
  3. *Authentication.* Credentials are validated only at activation:
     `daemon/RDPSession.m` (`setupDisplayAndMediaForWidth:height:`) calls
     `daemon/Authenticator.m`, which verifies the password against the local
     OpenDirectory node (`/Local/Default`, `ODRecord verifyPassword:`) and
     falls back to `/usr/bin/dscl . -authonly <user> <pass>` only if
     OpenDirectory is unreachable (the password is then visible in that
     child's `argv` to root). **Any local account that has a password can log
     in; there is no group or user allow-list.** `RDP_ALLOW_NO_AUTH=1` skips the
     check entirely. A failed check sets the session to disconnecting; no
     display, capture, input or clipboard objects exist until the check passes
     (`rdp_on_clipboard` messages a nil `clipboard` before that point).
  4. *Session takeover.* A newly authenticated client supersedes the current
     active session (`RDPServer sessionDidAuthenticateAndRequestActivation:`).
     A stalled or half-open connection is dropped after a 20 s negotiation
     watchdog (`RDPSession.m`, `kNegotiationTimeoutSecs`).

  Two consequences of step 2 deserve explicit mention:

  - With `RDP_RDPDR_ENABLED=1` (set by `launchd/com.macosrdp.agent.plist`), an
    **unauthenticated** client can complete the rdpdr handshake and send
    `DEVICE_LIST_ANNOUNCE`. For every announced drive the daemon creates
    `~/Desktop/RDP-Drives/<name>/README.txt` (`protocol/RDPDriveMount.m`),
    starts a loopback WebDAV server on `127.0.0.1:8760+n`
    (`rdp_webdav_server_create`), runs `/sbin/mount_webdav` onto
    `/Volumes/RDP-<name>` and registers an `NSFileProviderDomain`. The 8-byte
    `PreferredDosName` from the wire is used in those paths without
    sanitisation (only trailing spaces are stripped). The WebDAV servers are
    torn down in `context_free` when the session ends.
  - The loopback WebDAV servers have **no authentication of their own**: any
    local process on the Mac can read and write the connected client's
    redirected drive through `http://127.0.0.1:876x/` while the session is up.

- **Parsing of untrusted input.** The TPKT/X.224/MCS/fast-path/channel
  framing is FreeRDP's. The daemon's own wire parsing is limited to the rdpdr
  PDUs and `FILE_FULL_DIR_INFORMATION` chains in `protocol/RDPDRParse.c`
  (unit-tested and fuzzed under `tests/`), the MS-RDPEAI header check in
  `audio/AudioInput.m`, and the HTTP/WebDAV request parser in
  `protocol/RDPWebDAV.c` (reachable only from loopback). Findings against the
  FreeRDP fork are tracked in `docs/security-audit-2026-06-02.md`.

- **Auto-update (supply chain).** `daemon/AutoUpdate.m` polls
  `https://api.github.com/repos/<RDP_UPDATE_REPO>/releases/latest` (default
  `grioghar/macos-rdp-server`) every `RDP_UPDATE_INTERVAL_MIN` minutes
  (default 5), downloads the `macos-rdp-daemon` asset, requires a SHA-256 from
  the `macos-rdp-daemon.sha256` asset or the release body and refuses the
  binary if it is absent or does not match, re-signs it with the local
  self-signed identity (see below), replaces itself and restarts via
  `launchctl kickstart`. The checksum lives in the same GitHub release as the
  binary, so it protects against corrupt downloads, not against a compromised
  repository or token: **whoever can publish a release in that repository can
  run code as the daemon's user on every installed Mac within minutes.**
  `.github/workflows/ci.yml` publishes such a release for every push to
  `master`. Disable with `RDP_UPDATE_ENABLED=0`. An optional
  `RDP_UPDATE_TOKEN` is sent as a `Bearer` header to GitHub.
- **Installation.** The documented one-liner pipes
  `scripts/remote-install.sh` from the `master` branch into `sudo bash`
  (`README.md`). The same trust consideration as the updater applies.

### TLS certificate handling

- **Location.** `protocol/RDPPeer.c` (`peer_load_certificate`) reads
  `$RDP_CERT_DIR/server.key` and `$RDP_CERT_DIR/server.crt`; `RDP_CERT_DIR`
  defaults to `/etc/macos-rdp` (`RDP_CERT_DIR_DEFAULT`). The LaunchAgent
  variant sets `RDP_CERT_DIR` to `~/.macos-rdp` so the user-level daemon can
  read it.
- **Generation.** All certificates are **self-signed, generated at install
  time, never by the daemon itself**:
  - `scripts/gen-tls-cert.sh` and `scripts/remote-install.sh`:
    `openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes`,
    subject `/CN=<hostname>/O=macOS RDP/C=US`, SAN `DNS:<hostname>,IP:127.0.0.1`
    when the local OpenSSL supports `-addext` (otherwise no SAN);
    `/etc/macos-rdp` is `chmod 700`, `server.key` `600`, `server.crt` `644`.
    `gen-tls-cert.sh` prints the SHA-256 fingerprint once, at generation.
  - `scripts/install-user.sh`: `rsa:2048`, no SAN, `CN=<hostname>`, key
    `600` under `~/.macos-rdp`.
  - The private key is stored unencrypted (`-nodes`). Validity is 10 years;
    there is no rotation, renewal or revocation logic.
- **Loading.** The key and certificate are handed to FreeRDP via
  `FreeRDP_RdpServerRsaKey` / `FreeRDP_RdpServerCertificate`. If either file
  is missing, `peer_load_certificate` logs an error but `peer_apply_settings`
  ignores its return value; the daemon keeps listening and every TLS handshake
  then fails. The same RSA key is what FreeRDP uses for the Standard RDP
  Security key exchange if a client negotiates that layer.
- **Trust.** Nothing installs the certificate into a keychain or publishes
  its fingerprint. Clients are expected to trust it on first use (the README
  tells users to accept the prompt; `xfreerdp` is shown with `/cert:ignore`).
  A first connection over an untrusted network is therefore exposed to an
  active man-in-the-middle, and clients that silently accept any certificate
  are exposed on every connection.
- **Not to be confused with the code-signing certificate.**
  `scripts/install-user.sh` also creates a separate self-signed *code-signing*
  identity in `~/.macos-rdp/signing/macos-rdp.keychain-db` so that TCC grants
  survive updates. That keychain's password is the fixed string `macosrdp`
  (`KCPASS` in the script, `DEF_SIGN_PW` in `daemon/AutoUpdate.m`,
  overridable with `RDP_SIGN_KEYCHAIN_PW`). It is not a secret and does not
  protect anything beyond the local user's own files.

### DriverKit HID permissions

- `hid-driver/` contains a DriverKit system extension (`RDPHIDDriver`, an
  `IOUserHIDDevice`, bundle id `com.macosrdp.hid-driver`) whose purpose is to
  inject keyboard and mouse reports at the HID level so that input also works
  at the login window and in Secure Keyboard Entry fields.
- Its entitlements (`entitlements/hid-driver.entitlements`) are
  `com.apple.developer.driverkit`,
  `com.apple.developer.driverkit.family.hid.device`,
  `com.apple.developer.driverkit.family.hid.eventservice` and
  **`com.apple.developer.driverkit.allow-any-userclient-access`**. The last one
  means that, if the extension were activated, *any* process on the Mac, not
  only the daemon, could open its user client and inject input. The daemon's
  own entitlement file lists `com.apple.developer.driverkit.userclient-access`
  for `com.macosrdp.hid-driver` and
  `com.apple.developer.system-extension.install`.
- **Current status: not built, not activated, not used.** `CMakeLists.txt`
  has no target for `hid-driver/`, no daemon source opens an `IOUserClient`
  (input goes through `CGEventPost`; `daemon/DisplayControl.m` only creates a
  listen-only `CGEventTap`), and `scripts/install.sh` merely prints
  instructions to build, sign with an Apple Developer identity and run
  `systemextensionsctl activate com.macosrdp.hid-driver`. `docs/no-signing.md`
  documents the extension as optional and signing-only. Until that changes the
  DriverKit code adds no runtime attack surface.
  <!-- PLACEHOLDER(greg): is the DEXT planned to ship? If so, decide whether allow-any-userclient-access can be dropped in favour of a specific userclient-access grant. -->

### TCC grants and entitlements

What the daemon actually requests or needs, per the code:

| Grant | Why | Where |
|---|---|---|
| **Screen Recording** (`kTCCServiceScreenCapture`) | ScreenCaptureKit `SCStream` capture of the virtual display | `display/ScreenCapture.m` (`CGRequestScreenCaptureAccess()` prompt), `daemon/main.m --check-permissions` (`CGPreflightScreenCaptureAccess`) |
| **Accessibility** (`kTCCServiceAccessibility`) | `CGEventPost` keyboard/mouse injection; the listen-only `CGEventTap` in `DisplayControl.m` (its error text says Accessibility/Input Monitoring) | `input/InputInjector.m`, `daemon/DisplayControl.m`, `daemon/main.m` (`AXIsProcessTrusted`) |
| **System audio recording** | `AudioHardwareCreateProcessTap` with a global `CATapDescription` (system-audio tap); the code's own error text says "check TCC audio-recording permission" and the README calls this the Microphone grant | `audio/AudioCapture.m` |

<!-- PLACEHOLDER(greg): confirm which Privacy & Security pane actually gates the CATap on the macOS versions you target (Microphone vs. "System Audio Recording Only"). -->

- **Granting.** `scripts/grant-permissions.sh` and `scripts/remote-install.sh`
  open the Screen Recording and Accessibility panes. If SIP is disabled they
  instead write rows for the binary path into the system TCC database
  (`/Library/Application Support/com.apple.TCC/TCC.db`, `client_type=1`), which
  is exactly the kind of tampering SIP exists to prevent; `docs/no-signing.md`
  describes that path as optional.
- **Grant identity.** TCC pins grants to the binary's code identity.
  `scripts/install-user.sh` therefore signs the binary with a stable
  self-signed identity and the fixed identifier `macos-rdp-daemon`, and
  `daemon/AutoUpdate.m` re-signs every downloaded update with the same identity
  so the grants persist. An unsigned build (the CMake target does not sign)
  loses its grants on every rebuild.
- **Entitlements are declared but not applied.** `entitlements/daemon.entitlements`
  lists `com.apple.developer.virtual-display`,
  `com.apple.security.screen-recording`,
  `com.apple.security.automation.apple-events`,
  `com.apple.security.device.audio-input`,
  `com.apple.developer.system-extension.install`, the DriverKit user-client
  grant and hardened-runtime restrictions (JIT, unsigned executable memory
  and library validation all disabled). However, nothing in the repository
  passes it to `codesign`: `CMakeLists.txt` does not sign, and both
  `scripts/install-user.sh` and `AutoUpdate.m` invoke `codesign` without
  `--entitlements`. The shipped daemon therefore runs **without** the hardened
  runtime and without app-sandbox restrictions, relying only on TCC.
  <!-- PLACEHOLDER(greg): confirm whether the entitlements file is meant to be applied (it would need an Apple Developer identity for the com.apple.developer.* keys) or is documentation only. -->
- **No Full Disk Access, no Input Monitoring** are requested by the code;
  drive redirection writes only under `~/Desktop/RDP-Drives` and `/Volumes`.
- **Privilege.** Under the root LaunchDaemon all of the above runs as root;
  under the LaunchAgent it runs as the logged-in user and inherits that user's
  grants.

### Configuration flags that change the security posture

| Variable | Effect |
|---|---|
| `RDP_ALLOW_NO_AUTH=1` | Disables credential checking entirely. Never set this on a reachable network. |
| `RDP_RDPDR_ENABLED=1` | Enables drive redirection, including the pre-authentication rdpdr handshake and the unauthenticated loopback WebDAV servers described above. |
| `RDP_AUDIO_INPUT=1` | Opens the MS-RDPEAI channel in `peer_post_connect` and starts a CoreAudio output queue at once; `rdp_peer_run_once` drains DATA PDUs into it without checking `activated`, so an unauthenticated client can play audio through the Mac's speakers. |
| `RDP_UPDATE_ENABLED=0` | Turns off the self-updater (recommended if you do not want GitHub Releases to be a code-execution path). |
| `RDP_UPDATE_REPO`, `RDP_UPDATE_TOKEN` | Change which repository is trusted for updates / authenticate to it. |
| `RDP_CERT_DIR` | Where the TLS key and certificate are read from. |
| `RDP_SIGN_KEYCHAIN`, `RDP_SIGN_KEYCHAIN_PW`, `RDP_SIGN_IDENTITY` | Which local identity re-signs updates. |
| `RDP_LOG_LEVEL=debug` | Logs every key press and input event. Usernames and client addresses are logged at every level; passwords are never logged. |

## Reporting a vulnerability

Please do **not** open a public GitHub issue for security problems.

<!-- PLACEHOLDER(greg): security contact -->

When reporting, include: the daemon version (`macos-rdp-daemon --help`
prints usage; the version is logged at startup as `macos-rdp-daemon <version>
(build <sha>)`), how the daemon was installed (LaunchDaemon as root or
per-user LaunchAgent), the relevant `RDP_*` environment variables, the client
used, and a minimal reproduction (a PDU capture or a fuzz-crash input from
`tests/fuzz/fuzz_pdu.cc` is ideal for parser bugs).

<!-- PLACEHOLDER(greg): expected acknowledgement / fix timeline and disclosure policy -->

Issues in the vendored FreeRDP fork should also be reported upstream to the
FreeRDP project once a fix is available here; `docs/security-audit-2026-06-02.md`
records which fork patches are already filed upstream.
