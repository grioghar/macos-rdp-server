#define RDP_LOG_COMPONENT "security"
#include "logging/RDPLog.h"
#include "protocol/RDPSecurity.h"

#include <stdio.h>
#include <string.h>

#ifdef MACOS_RDP_ENABLE_NLA
#include <openssl/provider.h>
#include <openssl/evp.h>
#endif

#ifndef RDP_CREDENTIALS_PATH
#define RDP_CREDENTIALS_PATH "/etc/macos-rdp/credentials"
#endif

static bool g_md4_available = false;

bool rdp_security_init_crypto(void) {
#ifdef MACOS_RDP_ENABLE_NLA
    static bool done = false;
    if (done)
        return g_md4_available;
    done = true;

#ifdef MACOS_RDP_STATIC_LEGACY
    /* Fully static OpenSSL: there is no loadable legacy.dylib, so register the
     * provider built into providers/liblegacy.a (which must be on the link
     * line) before loading it by name. ossl_legacy_provider_init is OpenSSL's
     * documented static-provider entry point. */
    extern OSSL_provider_init_fn ossl_legacy_provider_init;
    OSSL_PROVIDER_add_builtin(NULL, "legacy", ossl_legacy_provider_init);
#endif

    /* default must be loaded explicitly once any provider is loaded manually. */
    OSSL_PROVIDER_load(NULL, "default");
    OSSL_PROVIDER_load(NULL, "legacy");

    EVP_MD *md4 = EVP_MD_fetch(NULL, "MD4", NULL);
    if (md4) {
        g_md4_available = true;
        EVP_MD_free(md4);
        rdp_info("OpenSSL legacy provider active — NTLM/MD4 available, NLA can be enforced");
    } else {
        rdp_error("MD4 unavailable after loading the legacy provider — NLA cannot be enforced");
    }
    return g_md4_available;
#else
    return false;
#endif
}

bool rdp_security_md4_available(void) {
    return g_md4_available;
}

static bool is_hex32(const char *s) {
    if (strlen(s) != 32)
        return false;
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

bool rdp_security_load_credentials(struct rdp_credentials *out) {
    if (!out)
        return false;

    FILE *f = fopen(RDP_CREDENTIALS_PATH, "r");
    if (!f)
        return false;

    memset(out, 0, sizeof(*out));
    char line[512];
    bool have_user = false, have_hash = false;

    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl)
            *nl = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "username") == 0 && val[0] != '\0') {
            strncpy(out->username, val, sizeof(out->username) - 1);
            have_user = true;
        } else if (strcmp(key, "passwordhash") == 0 && is_hex32(val)) {
            memcpy(out->password_hash, val, 32);
            out->password_hash[32] = '\0';
            have_hash = true;
        }
    }

    fclose(f);
    return have_user && have_hash;
}
