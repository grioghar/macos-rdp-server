#pragma once
/*
 * Authentication / crypto helpers for the RDP server.
 *
 * NLA (CredSSP) requires NTLM, whose MD4/RC4/DES live in OpenSSL 3.x's *legacy*
 * provider. The full authenticated path is compiled only when the build defines
 * MACOS_RDP_ENABLE_NLA (CMake option, default OFF) and links an OpenSSL that
 * exposes the legacy provider. See docs/enabling-nla.md.
 *
 * In a default build every function below is a safe no-op, so behaviour is
 * unchanged: TLS (+ legacy RDP) with no authentication.
 */
#include <stdbool.h>

struct rdp_credentials {
    char username[256];
    char password_hash[33]; /* 32 lowercase hex chars + NUL (NTLM hash) */
};

/* Activate the OpenSSL providers NTLM needs (default + legacy) and probe MD4.
 * Idempotent; safe to call from multiple connections. Returns true if MD4 (and
 * therefore NLA) is available. Always returns false in a non-NLA build. */
bool rdp_security_init_crypto(void);

/* Whether MD4/NTLM is available at runtime (result of rdp_security_init_crypto). */
bool rdp_security_md4_available(void);

/* Read the root-only credential file (default /etc/macos-rdp/credentials).
 * Returns true only if both a non-empty username and a 32-hex-char NTLM hash
 * are present. */
bool rdp_security_load_credentials(struct rdp_credentials *out);
