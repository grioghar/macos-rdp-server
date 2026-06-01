#define RDP_LOG_COMPONENT "peer"
#include "logging/RDPLog.h"
#include "protocol/RDPPeer.h"

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/server-common.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/ssl.h>
#include <winpr/synch.h>
#include <winpr/wtsapi.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* TLS material directory. Defaults to /etc/macos-rdp (root LaunchDaemon model);
 * override with RDP_CERT_DIR when running as a LaunchAgent in the user session,
 * where the cert/key live somewhere the user can read. */
#ifndef RDP_CERT_DIR_DEFAULT
#define RDP_CERT_DIR_DEFAULT "/etc/macos-rdp"
#endif

/* ── Settings ──────────────────────────────────────────────────────────── */

/* Load the server certificate + private key into the peer settings. Without
 * these, the TLS handshake cannot complete and the client aborts with a
 * connection/security error (mstsc 0x904). Returns false if either is missing. */
static bool peer_load_certificate(freerdp_peer *peer) {
    rdpSettings *s = peer->context->settings;

    const char *dir = getenv("RDP_CERT_DIR");
    if (!dir || !*dir) dir = RDP_CERT_DIR_DEFAULT;
    char keyPath[1024], certPath[1024];
    snprintf(keyPath,  sizeof(keyPath),  "%s/server.key", dir);
    snprintf(certPath, sizeof(certPath), "%s/server.crt", dir);

    rdpPrivateKey *key = freerdp_key_new_from_file(keyPath);
    if (!key) {
        rdp_error("could not load private key %s — run gen-tls-cert.sh", keyPath);
        return false;
    }
    if (!freerdp_settings_set_pointer_len(s, FreeRDP_RdpServerRsaKey, key, 1)) {
        rdp_error("failed to set server RSA key");
        return false;
    }

    rdpCertificate *cert = freerdp_certificate_new_from_file(certPath);
    if (!cert) {
        rdp_error("could not load certificate %s — run gen-tls-cert.sh", certPath);
        return false;
    }
    if (!freerdp_settings_set_pointer_len(s, FreeRDP_RdpServerCertificate, cert, 1)) {
        rdp_error("failed to set server certificate");
        return false;
    }

    rdp_verbose("loaded TLS certificate and key");
    return true;
}

static void peer_apply_settings(freerdp_peer *peer) {
    rdpSettings *s = peer->context->settings;
    /* Only set what we actually use. Every extra setting is dead weight. */
    freerdp_settings_set_bool(s,   FreeRDP_NetworkAutoDetect,       FALSE);
    freerdp_settings_set_bool(s,   FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_GfxH264,                 TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_GfxAVC444,               FALSE);
    freerdp_settings_set_bool(s,   FreeRDP_GfxSmallCache,           FALSE);
    freerdp_settings_set_bool(s,   FreeRDP_GfxThinClient,           FALSE);
    freerdp_settings_set_bool(s,   FreeRDP_RemoteFxCodec,           FALSE);
    /* Security: match the known-good sample-server config. Crucially disable
     * NLA — it requires NTLM/md4, which our minimal OpenSSL build omits (the
     * "md4 NTLM support not available" log line). Offer TLS (+ legacy RDP as a
     * fallback) so mstsc negotiates TLS. */
    freerdp_settings_set_bool(s,   FreeRDP_UseRdpSecurityLayer,     FALSE);
    freerdp_settings_set_bool(s,   FreeRDP_RdpSecurity,             TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_TlsSecurity,             TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_NlaSecurity,             FALSE);
    freerdp_settings_set_uint32(s, FreeRDP_TlsSecLevel,             1);
    freerdp_settings_set_uint32(s, FreeRDP_ColorDepth,              32);
    freerdp_settings_set_bool(s,   FreeRDP_UnicodeInput,            TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_HasHorizontalWheel,      TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_HasExtendedMouseEvent,   TRUE);
    freerdp_settings_set_bool(s,   FreeRDP_SoundBeepsEnabled,       FALSE);

    peer_load_certificate(peer);
    rdp_debug("peer settings applied");
}

/* ── Context lifecycle ─────────────────────────────────────────────────── */

static BOOL context_new(freerdp_peer *peer, rdpContext *ctx) {
    RDPPeerContext *c = (RDPPeerContext *)ctx;
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&c->xportLock, &mattr);
    pthread_mutexattr_destroy(&mattr);
    /* Open the Virtual Channel Manager — all dynamic channels live under it. */
    c->vcm = WTSOpenServerA((LPSTR)peer->context);
    if (!c->vcm || c->vcm == INVALID_HANDLE_VALUE) {
        rdp_error("WTSOpenServerA failed");
        return FALSE;
    }
    rdp_debug("VCM opened");
    return TRUE;
}

static void context_free(freerdp_peer *peer, rdpContext *ctx) {
    (void)peer;
    RDPPeerContext *c = (RDPPeerContext *)ctx;
    if (c->gfx)    { rdpgfx_server_context_free(c->gfx);    c->gfx    = NULL; }
    if (c->cliprdr){ cliprdr_server_context_free(c->cliprdr);c->cliprdr= NULL; }
    if (c->rdpsnd) { rdpsnd_server_context_free(c->rdpsnd);  c->rdpsnd = NULL; }
    if (c->vcm)    { WTSCloseServer(c->vcm);                 c->vcm    = NULL; }
    if (c->clipData) { free(c->clipData); c->clipData = NULL; c->clipLen = 0; }
    pthread_mutex_destroy(&c->xportLock);
    rdp_debug("peer context freed");
}

/* ── Input callbacks ───────────────────────────────────────────────────── */

static BOOL peer_keyboard(rdpInput *input, UINT16 flags, UINT8 code) {
    RDPPeerContext *ctx = (RDPPeerContext *)input->context;
    if (ctx->callbacks.onKeyboard)
        ctx->callbacks.onKeyboard(ctx->callbacks.userdata, flags, code);
    return TRUE;
}

static BOOL peer_mouse(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    RDPPeerContext *ctx = (RDPPeerContext *)input->context;
    if (ctx->callbacks.onMouse)
        ctx->callbacks.onMouse(ctx->callbacks.userdata, flags, x, y);
    return TRUE;
}

static BOOL peer_mouse_ex(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    RDPPeerContext *ctx = (RDPPeerContext *)input->context;
    if (ctx->callbacks.onMouseEx)
        ctx->callbacks.onMouseEx(ctx->callbacks.userdata, flags, x, y);
    return TRUE;
}

/* ── GFX caps negotiation ──────────────────────────────────────────────── */

static UINT gfx_caps_advertise(RdpgfxServerContext *gfx,
                                const RDPGFX_CAPS_ADVERTISE_PDU *pdu) {
    RDPPeerContext *ctx = (RDPPeerContext *)gfx->custom;
    rdp_verbose("GFX caps advertise: %u sets", pdu->capsSetCount);

    /* Stack-allocate the capset — capsSet is a pointer in 3.x. */
    RDPGFX_CAPSET capset = {0};
    RDPGFX_CAPS_CONFIRM_PDU confirm = {0};
    confirm.capsSet = &capset;

    /* Walk advertised caps: prefer 8.x (has AVC420), skip anything lower. */
    for (UINT16 i = 0; i < pdu->capsSetCount; i++) {
        rdp_debug("  caps[%u] version=0x%08x flags=0x%08x",
                  i, pdu->capsSets[i].version, pdu->capsSets[i].flags);
        /* Pick the HIGHEST advertised version >= v8 and echo it back VERBATIM
         * (version, length AND flags). Two mstsc requirements learned the hard way
         * from its client event log:
         *   1. The confirmed capset must EXACTLY match one advertised — leaving
         *      length=0 got CapsConfirm rejected (GfxEventConfirmCapsFailed /
         *      0x8007000D), so we copy the whole struct verbatim.
         *   2. Confirming the LOWEST version (v8, flags=0) leaves "AVC available: 0"
         *      and mstsc cannot decode our AVC420 frames (GfxEventDecodingW2S1PduFailed
         *      / 0x8000FFFF). v8.1 / v10.x enable AVC420 by default, so pick the
         *      highest version the client offered. */
        if (pdu->capsSets[i].version >= RDPGFX_CAPVERSION_8 &&
            pdu->capsSets[i].version >= capset.version) {
            capset = pdu->capsSets[i];
        }
    }
    if (!capset.version) {
        rdp_error("client advertised no usable GFX caps version");
        return ERROR_INTERNAL_ERROR;
    }
    rdp_verbose("GFX confirming version=0x%08x flags=0x%08x",
                capset.version, capset.flags);

    UINT rc = gfx->CapsConfirm(gfx, &confirm);
    if (rc != CHANNEL_RC_OK) { rdp_error("CapsConfirm failed: %u", rc); return rc; }

    rdpSettings *s = ctx->base.peer->context->settings;
    UINT32 w = freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth);
    UINT32 h = freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);

    /* NOTE: we deliberately do NOT send RDPGFX_RESET_GRAPHICS. Field testing showed
     * mstsc resets the connection ~10ms after receiving our RESET_GRAPHICS PDU
     * (before any surface frame), whereas without it the session is stable. The
     * surface is created at the full desktop size and mapped to output origin (0,0),
     * which mstsc composites against the negotiated desktop dimensions directly.
     * (The earlier black screen was the P-frame-before-keyframe bug, since fixed,
     * not a missing canvas.) If multi-monitor support is added later, RESET_GRAPHICS
     * will be needed — but the MONITOR_DEF format must be validated against mstsc. */

    RDPGFX_CREATE_SURFACE_PDU cs = {0};
    cs.surfaceId   = ctx->surfaceId;
    cs.width       = (UINT16)w;
    cs.height      = (UINT16)h;
    cs.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
    gfx->CreateSurface(gfx, &cs);

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU ms = {0};
    ms.surfaceId     = ctx->surfaceId;
    ms.outputOriginX = 0;
    ms.outputOriginY = 0;
    gfx->MapSurfaceToOutput(gfx, &ms);

    ctx->gfxReady = true;
    /* Any frames encoded during GFX setup were dropped ("gfx not ready"), so demand
     * a fresh IDR — the first frame the client decodes must be a keyframe. */
    ctx->sentKeyframe = false;
    if (ctx->callbacks.onKeyframeRequest) {
        ctx->keyframeRequested = true;
        ctx->callbacks.onKeyframeRequest(ctx->callbacks.userdata);
    }
    rdp_info("GFX pipeline ready (%ux%u AVC420)", w, h);
    return CHANNEL_RC_OK;
}

/* ── Clipboard callbacks ───────────────────────────────────────────────── */

/* The client sent us ITS Clipboard Capabilities (MS-RDPECLIP step 3). This is
 * the first proof mstsc accepted our caps + monitor-ready and is engaging the
 * channel. We just log it; the channel layer has already latched the negotiated
 * flags (e.g. useLongFormatNames) onto the context. */
static UINT cliprdr_client_capabilities(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_CAPABILITIES *caps) {
    (void)cliprdr;
    UINT32 flags = 0, version = 0;
    for (UINT32 i = 0; i < caps->cCapabilitiesSets; i++) {
        const CLIPRDR_CAPABILITY_SET *set = &caps->capabilitySets[i];
        if (set->capabilitySetType == CB_CAPSTYPE_GENERAL) {
            const CLIPRDR_GENERAL_CAPABILITY_SET *g =
                (const CLIPRDR_GENERAL_CAPABILITY_SET *)set;
            version = g->version;
            flags   = g->generalFlags;
        }
    }
    rdp_verbose("clipboard: <- ClientCapabilities (%u sets, version=0x%08x "
                "generalFlags=0x%08x longNames=%d)",
                caps->cCapabilitiesSets, version, flags,
                (flags & CB_USE_LONG_FORMAT_NAMES) ? 1 : 0);
    return CHANNEL_RC_OK;
}

/* The client ACKed our Format List (our Mac->Win advertise). On success it will
 * follow up with a Format Data Request when the user pastes on Windows. */
static UINT cliprdr_client_format_list_response(
        CliprdrServerContext *cliprdr,
        const CLIPRDR_FORMAT_LIST_RESPONSE *resp) {
    (void)cliprdr;
    rdp_verbose("clipboard: <- ClientFormatListResponse (flags=0x%04x %s)",
                resp->common.msgFlags,
                (resp->common.msgFlags & CB_RESPONSE_OK) ? "OK" : "FAIL");
    return CHANNEL_RC_OK;
}

/* Windows copied something: the client advertises the formats it now holds. We
 * must (a) ACK the list, then (b) PULL the bytes we care about by sending a Format
 * Data Request — the client never pushes data unsolicited. We only consume text,
 * so pick CF_UNICODETEXT (preferred, lossless UTF-16) and fall back to CF_TEXT. */
static UINT cliprdr_client_format_list(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_FORMAT_LIST *list) {
    RDPPeerContext *ctx = (RDPPeerContext *)cliprdr->custom;
    rdp_verbose("clipboard: <- ClientFormatList (%u formats)", list->numFormats);
    for (UINT32 i = 0; i < list->numFormats; i++)
        rdp_verbose("clipboard:    format[%u] id=0x%08x name=%s", i,
                    list->formats[i].formatId,
                    list->formats[i].formatName ? list->formats[i].formatName : "(none)");

    /* Acknowledge the advertisement first (msgType is set by the server serializer,
     * but a Format List RESPONSE carries CB_RESPONSE_OK in msgFlags). */
    CLIPRDR_FORMAT_LIST_RESPONSE resp = {0};
    resp.common.msgType  = CB_FORMAT_LIST_RESPONSE;
    resp.common.msgFlags = CB_RESPONSE_OK;
    cliprdr->ServerFormatListResponse(cliprdr, &resp);

    /* Choose the best text format the client offers. */
    UINT32 want = 0;
    for (UINT32 i = 0; i < list->numFormats; i++) {
        UINT32 id = list->formats[i].formatId;
        if (id == CF_UNICODETEXT) { want = CF_UNICODETEXT; break; }
        if (id == CF_TEXT)        { want = CF_TEXT; }
    }
    if (!want) {
        rdp_verbose("clipboard: client offered no text format — ignoring");
        return CHANNEL_RC_OK;
    }

    /* Request the bytes. The response (cliprdr_client_format_data) has no format
     * id of its own, so stash what we asked for. */
    ctx->clipReqFormat = want;
    CLIPRDR_FORMAT_DATA_REQUEST dreq = {0};
    dreq.common.msgType   = CB_FORMAT_DATA_REQUEST;
    dreq.requestedFormatId = want;
    rdp_verbose("clipboard: requesting format 0x%08x from client", want);
    cliprdr->ServerFormatDataRequest(cliprdr, &dreq);
    return CHANNEL_RC_OK;
}

/* The client answered our Format Data Request: hand the bytes to the Mac
 * pasteboard via the onClipboard callback. The response carries no format id, so
 * we use the one we requested (clipReqFormat). */
static UINT cliprdr_client_format_data(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_FORMAT_DATA_RESPONSE *resp) {
    RDPPeerContext *ctx = (RDPPeerContext *)cliprdr->custom;
    rdp_verbose("clipboard: <- ClientFormatDataResponse (flags=0x%04x len=%u)",
                resp->common.msgFlags, resp->common.dataLen);
    if ((resp->common.msgFlags & CB_RESPONSE_OK) && ctx->callbacks.onClipboard) {
        rdp_verbose("clipboard: %u bytes from client (format 0x%08x)",
                    resp->common.dataLen, ctx->clipReqFormat);
        ctx->callbacks.onClipboard(ctx->callbacks.userdata,
                                   resp->requestedFormatData,
                                   resp->common.dataLen,
                                   ctx->clipReqFormat ? ctx->clipReqFormat
                                                      : (UINT32)CF_UNICODETEXT);
    } else {
        rdp_verbose("clipboard: client format-data response failed (flags=0x%04x)",
                    resp->common.msgFlags);
    }
    return CHANNEL_RC_OK;
}

/* Client pasted: it requests the bytes for a format we advertised. Reply with
 * the held host data (or an empty failure response if we have none). */
static UINT cliprdr_client_format_data_request(
        CliprdrServerContext *cliprdr,
        const CLIPRDR_FORMAT_DATA_REQUEST *req) {
    RDPPeerContext *ctx = (RDPPeerContext *)cliprdr->custom;
    CLIPRDR_FORMAT_DATA_RESPONSE resp = {0};
    rdp_verbose("clipboard: <- ClientFormatDataRequest (format 0x%08x)",
                req->requestedFormatId);

    if (ctx->clipData && ctx->clipLen > 0 &&
        req->requestedFormatId == ctx->clipFormat) {
        resp.common.msgFlags     = CB_RESPONSE_OK;
        resp.common.dataLen      = (UINT32)ctx->clipLen;
        resp.requestedFormatData = (BYTE *)ctx->clipData;
        rdp_verbose("clipboard: serving %zu bytes for format 0x%08x",
                    ctx->clipLen, req->requestedFormatId);
    } else {
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        rdp_verbose("clipboard: data request 0x%08x but nothing matching held "
                    "(have format 0x%08x, %zu bytes)",
                    req->requestedFormatId, ctx->clipFormat, ctx->clipLen);
    }
    cliprdr->ServerFormatDataResponse(cliprdr, &resp);
    return CHANNEL_RC_OK;
}

/* ── Audio activated callback ──────────────────────────────────────────── */

/* Preferred client playback rate (Hz) from RDP_AUDIO_RATE, or 0 for "auto".
 *
 * mstsc commonly PLAYS rdpsnd at its 44100 device rate even when it advertises
 * 48000, so a 48000-tagged stream sounds a semitone low (44100/48000 = 0.919 —
 * exactly the reported drop). Preferring 44100 makes captured-rate == play-rate
 * and removes the shift. Gated behind an env var so the rate can be A/B tested
 * on hardware without a rebuild:
 *   "44100" (default) → prefer 44100 stereo 16-bit PCM
 *   "48000"           → prefer 48000
 *   "auto"            → no preference; take the first compatible client format */
static uint32_t rdp_preferred_audio_rate(void) {
    const char *env = getenv("RDP_AUDIO_RATE");
    if (!env || !*env)               return 44100;  /* default */
    if (strcmp(env, "auto") == 0)    return 0;
    long v = strtol(env, NULL, 10);
    if (v == 44100 || v == 48000 || v == 22050) return (uint32_t)v;
    rdp_info("RDP_AUDIO_RATE=\"%s\" not recognized — defaulting to 44100", env);
    return 44100;
}

static void rdpsnd_activated(RdpsndServerContext *rdpsnd) {
    RDPPeerContext *ctx = (RDPPeerContext *)rdpsnd->data;

    /* ── Diagnostics: dump the FULL negotiation so the real client offer is
     * visible in the log (the human reads this to confirm/choose the rate). */
    rdp_info("===== rdpsnd negotiation: %u client format(s) advertised =====",
             (unsigned)rdpsnd->num_client_formats);
    for (UINT16 i = 0; i < rdpsnd->num_client_formats; i++) {
        const AUDIO_FORMAT *cf = &rdpsnd->client_formats[i];
        rdp_info("  client[%u]: %u Hz, %u ch, %u-bit, tag 0x%04x, "
                 "blockAlign %u, avgBytes %u",
                 (unsigned)i, (unsigned)cf->nSamplesPerSec,
                 (unsigned)cf->nChannels, (unsigned)cf->wBitsPerSample,
                 (unsigned)cf->wFormatTag, (unsigned)cf->nBlockAlign,
                 (unsigned)cf->nAvgBytesPerSec);
    }
    rdp_info("----- server advertised %u format(s) -----",
             (unsigned)rdpsnd->num_server_formats);
    for (size_t j = 0; j < rdpsnd->num_server_formats; j++) {
        const AUDIO_FORMAT *sf = &rdpsnd->server_formats[j];
        rdp_info("  server[%zu]: %u Hz, %u ch, %u-bit, tag 0x%04x",
                 j, (unsigned)sf->nSamplesPerSec, (unsigned)sf->nChannels,
                 (unsigned)sf->wBitsPerSample, (unsigned)sf->wFormatTag);
    }

    /* CRITICAL (pitch bug): rdpsnd_server_send_samples() does NOT resample. It
     * encodes the bytes we hand it (described by ctx->rdpsnd->src_format) and
     * tags the WAVE PDU with wFormatNo = selected_client_format — an index into
     * the CLIENT's format list. The client plays our bytes at the SELECTED
     * CLIENT FORMAT's nSamplesPerSec. If that rate differs from the rate we
     * actually produced the PCM at, the client plays it faster/slower → pitch
     * shift.
     *
     * Guarantee src == play rate by (a) selecting a client format at the
     * PREFERRED rate (RDP_AUDIO_RATE, default 44100 — mstsc's usual device
     * rate), (b) pointing src_format at that EXACT client format BEFORE calling
     * SelectFormat (SelectFormat snapshots src_format to compute its
     * bytes-per-frame), and (c) resampling the 48 kHz tap to that negotiated
     * rate in AudioCapture (it polls rdp_peer_get_audio_rate).
     *
     * Two passes: first try to match the preferred rate; if the client offers
     * no compatible format at that rate, fall back to the first compatible
     * format of any rate so audio still works (just possibly shifted). */
    const uint32_t preferred = rdp_preferred_audio_rate();
    rdp_info("audio rate preference: %s (RDP_AUDIO_RATE)",
             preferred ? (preferred == 44100 ? "44100" :
                          preferred == 48000 ? "48000" : "22050") : "auto");

    int chosen = -1;
    /* Pass 1: preferred rate (skipped when preferred == 0 / "auto"). */
    if (preferred) {
        for (UINT16 i = 0; i < rdpsnd->num_client_formats && chosen < 0; i++) {
            if (rdpsnd->client_formats[i].nSamplesPerSec != preferred) continue;
            for (size_t j = 0; j < rdpsnd->num_server_formats; j++) {
                if (audio_format_compatible(&rdpsnd->server_formats[j],
                                            &rdpsnd->client_formats[i])) {
                    chosen = (int)i;
                    break;
                }
            }
        }
        if (chosen < 0)
            rdp_info("no compatible client format at preferred %u Hz — "
                     "falling back to first compatible format", preferred);
    }
    /* Pass 2: first compatible format of any rate. */
    for (UINT16 i = 0; i < rdpsnd->num_client_formats && chosen < 0; i++) {
        for (size_t j = 0; j < rdpsnd->num_server_formats; j++) {
            if (audio_format_compatible(&rdpsnd->server_formats[j],
                                        &rdpsnd->client_formats[i])) {
                chosen = (int)i;
                break;
            }
        }
    }

    if (chosen < 0) {
        rdp_error("no compatible audio format found among %u client formats",
                  (unsigned)rdpsnd->num_client_formats);
        return;
    }

    const AUDIO_FORMAT *sel = &rdpsnd->client_formats[chosen];
    /* Point src_format at the selected client format BEFORE SelectFormat —
     * SelectFormat reads src_format to compute src bytes-per-frame, and it is
     * the format SendSamples describes the PCM with, so it MUST equal the
     * client format we tag the wire with (same rate/ch/bits ⇒ no implicit
     * reinterpretation / pitch shift). */
    rdpsnd->src_format = (AUDIO_FORMAT *)sel;
    UINT rc = rdpsnd->SelectFormat(rdpsnd, (UINT16)chosen);
    if (rc != CHANNEL_RC_OK) {
        rdp_error("SelectFormat(idx %d) failed rc=%u — audio disabled",
                  chosen, (unsigned)rc);
        return;
    }

    /* Confirm SelectFormat actually committed the index we passed: the wire
     * wFormatNo and rdp_peer_get_audio_rate both read selected_client_format,
     * so a mismatch here would mean we resample to one rate but tag another. */
    UINT16 committed = rdpsnd->selected_client_format;
    if (committed != (UINT16)chosen) {
        rdp_error("selected_client_format mismatch: passed %d but context holds "
                  "%u — wire wFormatNo would disagree with resample rate!",
                  chosen, (unsigned)committed);
    }

    ctx->audioReady = true;
    rdp_info("audio format negotiated: client idx %d (committed %u) — "
             "%u Hz, %u ch, %u-bit, tag 0x%04x — wire wFormatNo=%u, "
             "tap will resample to %u Hz (src=play rate)",
             chosen, (unsigned)committed,
             (unsigned)sel->nSamplesPerSec, (unsigned)sel->nChannels,
             (unsigned)sel->wBitsPerSample, (unsigned)sel->wFormatTag,
             (unsigned)committed, (unsigned)sel->nSamplesPerSec);
}

/* Negotiated client playback sample rate (Hz), or 0 if audio is not yet
 * activated. AudioCapture polls this so its resampler targets the exact rate
 * the client plays at — see the pitch-bug note in rdpsnd_activated. */
uint32_t rdp_peer_get_audio_rate(freerdp_peer *peer) {
    if (!peer || !peer->context) return 0;
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->rdpsnd || !ctx->audioReady) return 0;
    UINT16 idx = ctx->rdpsnd->selected_client_format;
    if (idx >= ctx->rdpsnd->num_client_formats) return 0;
    return (uint32_t)ctx->rdpsnd->client_formats[idx].nSamplesPerSec;
}

/* ── PostConnect: open virtual channels ───────────────────────────────── */

static BOOL peer_post_connect(freerdp_peer *peer) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    rdp_verbose("post-connect: opening virtual channels");

    /* GFX is a DYNAMIC virtual channel: it can only be opened once drdynvc has
     * reached DRDYNVC_STATE_READY, which does NOT happen until after PostConnect.
     * Opening here fails with "WTSVirtualChannelOpenEx failed" and the client
     * gets no video (black screen). Create the context now; defer Open() to the
     * run loop, which opens it when drdynvc is ready. */
    ctx->gfx = rdpgfx_server_context_new(ctx->vcm);
    if (!ctx->gfx) { rdp_error("rdpgfx_server_context_new failed"); return FALSE; }
    ctx->gfx->custom        = ctx;
    ctx->gfx->CapsAdvertise = gfx_caps_advertise;
    ctx->surfaceId          = 1;
    /* External-thread mode: do NOT let GFX spawn its own thread. The default
     * (ownThread=TRUE) runs an internal loop calling rdpgfx_server_handle_messages
     * — which would race our run-loop's own handle_messages call and the encoder
     * thread's SurfaceCommand on the shared send_stream/zgfx state (heap
     * corruption / SIGABRT). With external mode WE are the sole driver, and a
     * mutex serializes the run loop vs the encoder. */
    if (ctx->gfx->Initialize)
        ctx->gfx->Initialize(ctx->gfx, TRUE);
    rdp_verbose("GFX context created (external-thread mode); open deferred");

    /* Clipboard. */
    ctx->cliprdr = cliprdr_server_context_new(ctx->vcm);
    if (ctx->cliprdr) {
        ctx->cliprdr->custom                   = ctx;
        ctx->cliprdr->rdpcontext               = peer->context;
        /* Register EVERY inbound callback BEFORE Open() so no client PDU is
         * dropped, and so the log shows the full MS-RDPECLIP exchange. */
        ctx->cliprdr->ClientCapabilities       = cliprdr_client_capabilities;
        ctx->cliprdr->ClientFormatList         = cliprdr_client_format_list;
        ctx->cliprdr->ClientFormatListResponse = cliprdr_client_format_list_response;
        ctx->cliprdr->ClientFormatDataResponse = cliprdr_client_format_data;
        ctx->cliprdr->ClientFormatDataRequest  = cliprdr_client_format_data_request;
        ctx->cliprdr->useLongFormatNames       = TRUE;
        if (ctx->cliprdr->Open(ctx->cliprdr) != CHANNEL_RC_OK) {
            rdp_verbose("clipboard channel open failed");
            cliprdr_server_context_free(ctx->cliprdr);
            ctx->cliprdr = NULL;
        } else {
            /* We pump the channel via the shared VCM, NOT cliprdr's own Start()
             * thread — so the server-init handshake (Clipboard Capabilities +
             * Monitor Ready) is never sent automatically. Send it ourselves, or
             * the client never engages and copy/paste is dead BOTH directions. */
            CLIPRDR_GENERAL_CAPABILITY_SET general = {
                .capabilitySetType   = CB_CAPSTYPE_GENERAL,
                .capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN,
                .version             = CB_CAPS_VERSION_2,
                .generalFlags        = CB_USE_LONG_FORMAT_NAMES,
            };
            CLIPRDR_CAPABILITIES caps = {
                .common = { .msgType = CB_CLIP_CAPS, .msgFlags = 0,
                            .dataLen = 4 + CB_CAPSTYPE_GENERAL_LEN },
                .cCapabilitiesSets = 1,
                .capabilitySets = (CLIPRDR_CAPABILITY_SET *)&general,
            };
            CLIPRDR_MONITOR_READY ready = { .common = { .msgType = CB_MONITOR_READY } };
            UINT cc = ctx->cliprdr->ServerCapabilities(ctx->cliprdr, &caps);
            UINT mr = ctx->cliprdr->MonitorReady(ctx->cliprdr, &ready);
            rdp_verbose("clipboard channel opened (caps=%u monitor-ready=%u)", cc, mr);
        }
    }

    /* Audio. Advertise raw PCM stereo 16-bit at the standard rates mstsc
     * expects (48000 / 44100 / 22050). We DELIBERATELY offer only raw PCM, no
     * compressed codecs (AAC/ADPCM/GSM): our SendSamples feeds raw PCM, so a
     * negotiated codec would hand the client undecodable garbage.
     *
     * Why multiple rates (the pitch fix): rdpsnd_server_send_samples() does not
     * resample — the client plays our bytes at the SELECTED CLIENT FORMAT's
     * rate. mstsc frequently prefers 44100, so advertising only 48000 either
     * fails to match (silence) or, worse, ends up with the client replaying
     * 48000 bytes at 44100 → upward pitch shift. By offering the standard rates
     * we let a clean PCM format match; rdpsnd_activated then points src_format
     * at the selected client format and AudioCapture resamples the 48 kHz tap to
     * that exact rate (rdp_peer_get_audio_rate), so captured rate == play rate.
     *
     * rdpsnd_activated PREFERS the RDP_AUDIO_RATE rate (default 44100 — mstsc's
     * usual playback device rate) when the client offers it, falling back to the
     * first compatible format otherwise; list order here is no longer the
     * tie-breaker, but all three rates must be advertised so the preferred one
     * can match. Must be heap-allocated: rdpsnd_server_context_free() calls
     * free() on server_formats, so a static array would crash on teardown. */
    ctx->rdpsnd = rdpsnd_server_context_new(ctx->vcm);
    if (ctx->rdpsnd) {
        static const UINT32 kRates[] = { 48000, 44100, 22050 };
        const size_t nFmt = sizeof(kRates) / sizeof(kRates[0]);
        AUDIO_FORMAT *pcm = (AUDIO_FORMAT *)calloc(nFmt, sizeof(AUDIO_FORMAT));
        if (pcm) {
            for (size_t k = 0; k < nFmt; k++) {
                pcm[k].wFormatTag      = WAVE_FORMAT_PCM;
                pcm[k].nChannels       = 2;
                pcm[k].nSamplesPerSec  = kRates[k];
                pcm[k].nAvgBytesPerSec = kRates[k] * 2 * 2;
                pcm[k].nBlockAlign     = 2 * 2;
                pcm[k].wBitsPerSample  = 16;
            }
        }
        ctx->rdpsnd->data               = ctx;
        ctx->rdpsnd->Activated          = rdpsnd_activated;
        ctx->rdpsnd->server_formats     = pcm;
        ctx->rdpsnd->num_server_formats = pcm ? nFmt : 0;
        /* Initial src_format; repointed to the selected client format on
         * activation. Must be non-NULL before SelectFormat (it bounds-checks
         * src_format). */
        ctx->rdpsnd->src_format         = pcm;
        if (ctx->rdpsnd->Initialize(ctx->rdpsnd, TRUE) != CHANNEL_RC_OK) {
            rdp_verbose("audio channel init failed");
            rdpsnd_server_context_free(ctx->rdpsnd);
            ctx->rdpsnd = NULL;
        } else { rdp_verbose("audio channel opened (PCM 48000/44100/22050)"); }
    }

    return TRUE;
}

/* Client minimized / restored its RDP window. mstsc sends Suppress Output
 * (allow=FALSE) on minimize and again (allow=TRUE) on restore. While suppressed
 * we must STOP sending graphics; on restore we must resume AND send a fresh
 * keyframe — the client discarded everything while minimized, so a delta would
 * reference frames it no longer has (black until the next IDR). Without this the
 * window comes back black. */
static BOOL peer_suppress_output(rdpContext *context, BYTE allow,
                                 const RECTANGLE_16 *area) {
    (void)area;
    RDPPeerContext *ctx = (RDPPeerContext *)context;
    if (allow) {
        ctx->outputSuppressed = false;
        /* Force a self-contained refresh so the restored window decodes. */
        ctx->sentKeyframe = false;
        if (ctx->callbacks.onKeyframeRequest) {
            ctx->keyframeRequested = true;
            ctx->callbacks.onKeyframeRequest(ctx->callbacks.userdata);
        }
        rdp_verbose("client output ALLOWED (restored) — forcing keyframe");
    } else {
        ctx->outputSuppressed = true;
        rdp_verbose("client output SUPPRESSED (minimized) — pausing frames");
    }
    return TRUE;
}

static BOOL peer_activate(freerdp_peer *peer) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    ctx->activated = true;
    rdpSettings *s = peer->context->settings;
    UINT32 w = freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth);
    UINT32 h = freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);
    UINT32 d = freerdp_settings_get_uint32(s, FreeRDP_ColorDepth);
    rdp_info("peer activated: %ux%u @%ubpp", w, h, d);

    /* Capture the logon credentials the client sent in the RDP info packet. NLA
     * is OFF (our OpenSSL build lacks md4/NTLM), so mstsc transmits them here in
     * the TLS-protected RDP logon. The session layer validates them against the
     * local macOS account (Authenticator) before granting/taking over a session.
     * NEVER log the password. Username/Domain are safe to log. */
    const char *user   = freerdp_settings_get_string(s, FreeRDP_Username);
    const char *domain = freerdp_settings_get_string(s, FreeRDP_Domain);
    rdp_info("logon credentials received: user=%s domain=%s (password %s)",
             user ? user : "(none)", domain ? domain : "(none)",
             freerdp_settings_get_string(s, FreeRDP_Password) ? "present" : "absent");

    if (ctx->callbacks.onReady)
        ctx->callbacks.onReady(ctx->callbacks.userdata, w, h, d);
    return TRUE;
}

/* Read the client's logon credentials from the negotiated peer settings. Valid
 * after peer_activate has run (Activate / onReady). The returned pointers are
 * owned by FreeRDP settings and live as long as the peer; any may be NULL when
 * the client supplied no value. NEVER log *password. */
void rdp_peer_get_credentials(freerdp_peer *peer,
                              const char **username,
                              const char **password,
                              const char **domain) {
    const char *u = NULL, *p = NULL, *dm = NULL;
    if (peer && peer->context) {
        rdpSettings *s = peer->context->settings;
        u  = freerdp_settings_get_string(s, FreeRDP_Username);
        p  = freerdp_settings_get_string(s, FreeRDP_Password);
        dm = freerdp_settings_get_string(s, FreeRDP_Domain);
    }
    if (username) *username = u;
    if (password) *password = p;
    if (domain)   *domain   = dm;
}

/* ── Public API ────────────────────────────────────────────────────────── */

/* Register FreeRDP's built-in server WTS implementation exactly once, before
 * the first WTSOpenServerA. Without this, WinPR's WTS layer tries to dlopen the
 * external FreeRDS plugin (libfreerds-fdsapi.so, Linux-only, absent on macOS),
 * WTSOpenServerA returns NULL, and every connection dies in context_new with
 * "ContextNew callback failed". The sample server does the same (sfreerdp.c).
 *
 * FreeRDP_InitWtsApi is exported by libfreerdp-server but only declared in an
 * internal header the public SDK does not ship, so we forward-declare it. The
 * signature matches WinPR's INIT_WTSAPI_FN typedef. */
extern const WtsApiFunctionTable *FreeRDP_InitWtsApi(void);

static pthread_once_t g_wts_once = PTHREAD_ONCE_INIT;
static void register_freerdp_wts(void) {
    WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi());
}

freerdp_peer *rdp_peer_create(int fd, const RDPPeerCallbacks *callbacks) {
    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
    pthread_once(&g_wts_once, register_freerdp_wts);
    rdp_verbose("creating peer for fd=%d", fd);

    freerdp_peer *peer = freerdp_peer_new(fd);
    if (!peer) { rdp_error("freerdp_peer_new failed"); return NULL; }

    /* ContextSize (PascalCase in 3.x) replaces context_size. */
    peer->ContextSize  = sizeof(RDPPeerContext);
    peer->ContextNew   = context_new;
    peer->ContextFree  = context_free;

    if (!freerdp_peer_context_new(peer)) {
        rdp_error("freerdp_peer_context_new failed");
        freerdp_peer_free(peer);
        return NULL;
    }

    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (callbacks) ctx->callbacks = *callbacks;

    peer_apply_settings(peer);
    peer->PostConnect = peer_post_connect;
    peer->Activate    = peer_activate;
    /* Handle minimize/restore (Suppress Output) so the window doesn't come back
     * black. SuppressOutput lives on rdpUpdate in 3.x. */
    peer->context->update->SuppressOutput = peer_suppress_output;

    /* Input is on rdpContext, not on freerdp_peer in 3.x. */
    peer->context->input->KeyboardEvent      = peer_keyboard;
    peer->context->input->MouseEvent         = peer_mouse;
    peer->context->input->ExtendedMouseEvent = peer_mouse_ex;

    if (!peer->Initialize(peer)) {
        rdp_error("peer Initialize failed (TLS handshake error)");
        freerdp_peer_context_free(peer);
        freerdp_peer_free(peer);
        return NULL;
    }

    rdp_verbose("peer initialized");
    return peer;
}

void rdp_peer_destroy(freerdp_peer *peer) {
    if (!peer) return;
    rdp_verbose("destroying peer");
    freerdp_peer_context_free(peer);
    freerdp_peer_free(peer);
}

/*
 * Event-driven run: waits on peer transport handles + VCM channel event
 * rather than spinning. Blocks up to 50ms then returns — caller checks
 * disconnect flag. This replaces the busy-poll loop with proper WaitForMultiple.
 */
bool rdp_peer_run_once(freerdp_peer *peer) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;

    HANDLE events[68] = {0};
    DWORD  nCount = 0;

    /* Peer transport events (typically 1-2 handles). */
    DWORD n = peer->GetEventHandles(peer, events, 64);
    if (n == 0) { rdp_error("GetEventHandles returned 0"); return false; }
    nCount += n;

    /* VCM channel event (drdynvc, cliprdr, etc.). */
    HANDLE vcmEvent = WTSVirtualChannelManagerGetEventHandle(ctx->vcm);
    if (vcmEvent) events[nCount++] = vcmEvent;

    /* GFX channel event (only once the DVC is actually open). */
    if (ctx->gfx && ctx->gfxOpened) {
        HANDLE gfxEvent = rdpgfx_server_get_event_handle(ctx->gfx);
        if (gfxEvent) events[nCount++] = gfxEvent;
    }

    /* Clipboard channel event. cliprdr is a STATIC virtual channel opened in
     * PostConnect, so its event handle is valid as soon as Open() succeeded —
     * no drdynvc dependency (unlike GFX). We pump it here via the shared run
     * loop instead of cliprdr's own Start() reader thread (which would race the
     * transport against this loop and the encoder thread). WITHOUT this wait +
     * the drain below, cliprdr_server_read is NEVER called, so the client's
     * ClientCapabilities / ClientFormatList / FormatDataRequest /
     * FormatDataResponse PDUs are never read off the wire — copy/paste is dead
     * BOTH directions even though our outbound caps/monitor-ready/format-list
     * were sent fine. THIS was the bug. */
    if (ctx->cliprdr) {
        HANDLE clipEvent = ctx->cliprdr->GetEventHandle(ctx->cliprdr);
        if (clipEvent) events[nCount++] = clipEvent;
    }

    DWORD status = WaitForMultipleObjects(nCount, events, FALSE, 50 /*ms*/);
    if (status == WAIT_FAILED) {
        rdp_error("WaitForMultipleObjects failed");
        return false;
    }

    /* Everything below WRITES to the transport (CheckFileDescriptor sends acks +
     * input responses, the VCM pump flushes channel PDUs incl. cliprdr, GFX
     * handle_messages drains/answers). Hold xportLock across the whole section so
     * none of it interleaves with the encoder thread's SurfaceCommand, the audio
     * thread's SendSamples, or the clipboard thread's ServerFormatList. The 50ms
     * Wait above is deliberately OUTSIDE the lock so we don't starve those threads
     * while idle. */
    bool ok = true;
    pthread_mutex_lock(&ctx->xportLock);

    /* Process peer transport data. */
    if (!peer->CheckFileDescriptor(peer)) {
        rdp_verbose("peer transport closed");
        ok = false;
    }

    /* Dispatch any pending virtual channel messages. This also advances the
     * drdynvc state machine toward READY. */
    if (ok && vcmEvent && WaitForSingleObject(vcmEvent, 0) == WAIT_OBJECT_0) {
        if (!WTSVirtualChannelManagerCheckFileDescriptor(ctx->vcm)) {
            rdp_error("VCM check failed");
            ok = false;
        }
    }

    /* Open the GFX dynamic virtual channel once drdynvc is READY. GFX cannot be
     * opened in PostConnect (drdynvc isn't up yet) — doing it here is how the
     * shadow server brings up DVCs. Without this the client gets no video. */
    if (ok && ctx->gfx && !ctx->gfxOpened &&
        WTSVirtualChannelManagerGetDrdynvcState(ctx->vcm) == DRDYNVC_STATE_READY) {
        if (ctx->gfx->Open(ctx->gfx)) {
            ctx->gfxOpened = true;
            rdp_info("GFX channel opened (drdynvc ready)");
        } else {
            rdp_error("GFX Open failed despite drdynvc ready");
        }
    }

    /* Drain GFX channel messages once the channel is open. */
    if (ok && ctx->gfx && ctx->gfxOpened) {
        HANDLE gfxEvent = rdpgfx_server_get_event_handle(ctx->gfx);
        if (gfxEvent && WaitForSingleObject(gfxEvent, 0) == WAIT_OBJECT_0) {
            rdpgfx_server_handle_messages(ctx->gfx);
        }
    }

    /* Drain clipboard channel messages. CheckEventHandle == cliprdr_server_read:
     * it reads one PDU off the static channel and dispatches it to our Client*
     * callbacks, which themselves write responses (FormatListResponse,
     * FormatDataRequest/Response) back over the transport — hence it MUST run
     * under xportLock, which we already hold here. This is what finally lets us
     * SEE the client engage the clipboard. */
    if (ok && ctx->cliprdr) {
        HANDLE clipEvent = ctx->cliprdr->GetEventHandle(ctx->cliprdr);
        if (clipEvent && WaitForSingleObject(clipEvent, 0) == WAIT_OBJECT_0) {
            UINT crc = ctx->cliprdr->CheckEventHandle(ctx->cliprdr);
            if (crc != CHANNEL_RC_OK)
                rdp_verbose("clipboard read failed: %u", crc);
        }
    }

    pthread_mutex_unlock(&ctx->xportLock);
    return ok;
}

bool rdp_peer_send_h264_frame(freerdp_peer *peer,
                               const uint8_t *data, size_t len,
                               uint32_t width, uint32_t height,
                               bool isKeyFrame,
                               uint16_t dirtyX, uint16_t dirtyY,
                               uint16_t dirtyW, uint16_t dirtyH) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->gfxReady || !ctx->gfx) { rdp_debug("gfx not ready"); return false; }

    /* Client minimized: it ignores (and may choke on) graphics while output is
     * suppressed. Drop frames until it restores (which forces a fresh keyframe). */
    if (ctx->outputSuppressed) return true;

    /* The first frame the client receives MUST be a self-contained keyframe.
     * Frames encoded while the GFX channel was still opening were discarded, so a
     * delta sent now references frames the client never got -> undecodable (black).
     * Drop deltas until a keyframe goes out, asking the encoder to emit one. */
    if (!ctx->sentKeyframe && !isKeyFrame) {
        if (!ctx->keyframeRequested && ctx->callbacks.onKeyframeRequest) {
            ctx->keyframeRequested = true;
            ctx->callbacks.onKeyframeRequest(ctx->callbacks.userdata);
            rdp_debug("delta before first keyframe — requested IDR, dropping");
        }
        return true;
    }
    if (isKeyFrame) { ctx->sentKeyframe = true; ctx->keyframeRequested = false; }

    /* Build the damage region. Keyframes refresh the whole surface, so they
     * must claim the full extent; inter-frames claim only the dirty rect.
     * Align to 16px macroblock boundaries and clamp to surface bounds — the
     * AVC420 region must lie within the decoded picture or the client rejects it. */
    RECTANGLE_16 rect;
    if (isKeyFrame || dirtyW == 0 || dirtyH == 0) {
        rect.left = 0; rect.top = 0;
        rect.right = (UINT16)width; rect.bottom = (UINT16)height;
    } else {
        uint32_t left   = dirtyX & ~15u;                 /* round down to 16 */
        uint32_t top    = dirtyY & ~15u;
        uint32_t right  = (dirtyX + dirtyW + 15u) & ~15u; /* round up to 16 */
        uint32_t bottom = (dirtyY + dirtyH + 15u) & ~15u;
        if (right  > width)  right  = width;
        if (bottom > height) bottom = height;
        if (left   >= right)  { left = 0; right = (uint32_t)width; }
        if (top    >= bottom) { top = 0;  bottom = (uint32_t)height; }
        rect.left = (UINT16)left;  rect.top = (UINT16)top;
        rect.right = (UINT16)right; rect.bottom = (UINT16)bottom;
    }

    /* quantQualityVals MUST be a valid parallel array — the server serializer
     * dereferences quantQualityVals[i] for every region rect. NULL crashes.
     * qp is a quality hint (QoE only, not decode-critical); qualityVal mirrors
     * FreeRDP's own encoder: 100 - (qp & 0x3F). */
    RDPGFX_H264_QUANT_QUALITY quant = {0};
    quant.qp         = 26;
    quant.qualityVal = (BYTE)(100 - (26 & 0x3F));

    RDPGFX_AVC420_BITMAP_STREAM avc = {0};
    avc.data                  = (BYTE *)data;
    avc.length                = (UINT32)len;
    avc.meta.numRegionRects   = 1;
    avc.meta.regionRects      = &rect;
    avc.meta.quantQualityVals = &quant;

    /* The H.264 picture is full-surface, so the destination extent is too. */
    RDPGFX_SURFACE_COMMAND cmd = {0};
    cmd.surfaceId = ctx->surfaceId;
    cmd.codecId   = RDPGFX_CODECID_AVC420;
    /* cmd.format is a FreeRDP COLOR format (color.h), not a GFX wire enum.
     * rdpgfx_write_surface_command only accepts BGRX32/BGRA32 (-> XRGB/ARGB
     * on the wire). Using the wire enum yields "Format UNKNOWN not supported". */
    cmd.format    = PIXEL_FORMAT_BGRX32;
    cmd.right     = (UINT16)width;
    cmd.bottom    = (UINT16)height;
    cmd.length    = (UINT32)len;
    cmd.data      = (BYTE *)data;
    cmd.extra     = &avc;

    /* Wrap the surface command in StartFrame/EndFrame markers, like the shadow
     * server. mstsc needs the frame boundaries (and frameId) to composite the
     * decoded surface to the display — bare SurfaceCommands often don't render. */
    uint32_t fid = ++ctx->frameId;
    RDPGFX_START_FRAME_PDU startFrame = {0};
    startFrame.frameId = fid;
    RDPGFX_END_FRAME_PDU endFrame = {0};
    endFrame.frameId = fid;

    /* Serialize against ALL other transport writers (run loop, cliprdr, audio). */
    pthread_mutex_lock(&ctx->xportLock);
    UINT rc = ctx->gfx->SurfaceFrameCommand(ctx->gfx, &cmd, &startFrame, &endFrame);
    pthread_mutex_unlock(&ctx->xportLock);
    if (rc != CHANNEL_RC_OK) { rdp_error("SurfaceFrameCommand failed: %u", rc); return false; }
    rdp_debug("sent %s frame: region=(%u,%u)-(%u,%u) len=%zu",
              isKeyFrame ? "key" : "delta",
              rect.left, rect.top, rect.right, rect.bottom, len);
    return true;
}

void rdp_peer_send_default_cursor(freerdp_peer *peer) {
    /* Without any pointer update, mstsc renders the cursor coupled to frame
     * redraws (jerky). Advertising the default SYSTEM pointer makes the client
     * draw + move the cursor locally at the mouse's native rate (smooth), like a
     * Windows RDP server. (Showing the actual Mac cursor shapes lag-free would
     * need full color-pointer PDUs built from the captured cursor — a follow-up.) */
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    rdpPointerUpdate *pointer = peer->context->update->pointer;
    if (!pointer || !pointer->PointerSystem) return;
    POINTER_SYSTEM_UPDATE sys = {0};
    sys.type = SYSPTR_DEFAULT;
    pthread_mutex_lock(&ctx->xportLock);
    pointer->PointerSystem(peer->context, &sys);
    pthread_mutex_unlock(&ctx->xportLock);
    rdp_verbose("sent default system pointer (client-side cursor)");
}

/* Cursor cap from CursorCapture (kMaxCursorDim). Anything <= 96px in EITHER
 * dimension still fits a New (color) pointer, which mstsc renders identically
 * to a Large pointer for these sizes; we use New for <=96 and Large beyond. */
#define RDP_CURSOR_NEW_MAX 96

void rdp_peer_send_cursor_shape(freerdp_peer *peer,
                                const uint8_t *bgra, uint32_t w, uint32_t h,
                                uint16_t hotX, uint16_t hotY) {
    if (!peer || !bgra || w == 0 || h == 0) return;
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;

    /* Don't push pointers before the client is up: pre-activation the update
     * channel isn't wired, and (like graphics) sends while output is suppressed
     * are pointless. Mirrors rdp_peer_send_h264_frame's readiness gate. */
    if (!ctx->activated) { rdp_debug("cursor: peer not activated"); return; }
    if (ctx->outputSuppressed) return;

    rdpPointerUpdate *pointer = peer->context->update->pointer;
    if (!pointer) return;

    /* Clamp hotspot inside the bitmap. */
    if (hotX >= w) hotX = (uint16_t)(w - 1);
    if (hotY >= h) hotY = (uint16_t)(h - 1);

    /* ── Build the XOR (color) mask ───────────────────────────────────────
     * mstsc renders 32bpp/alpha color pointers UNRELIABLY (it draws nothing —
     * the cursor is invisible even though the PDUs are well-formed). We
     * therefore emit a CLASSIC 24bpp BGR color pointer, which mstsc renders
     * reliably (MS-RDPBCGR 2.2.9.1.1.4.4). Layout: 3 bytes/pixel (B,G,R, no
     * alpha), scanlines BOTTOM-UP, each row padded to a 2-byte (WORD) boundary.
     * Our input is top-down BGRA premultiplied, so we emit rows in reverse and
     * drop the alpha byte. Because alpha is premultiplied, (semi-)transparent
     * pixels are already darkened toward black; the AND mask masks them out, so
     * the dropped alpha costs nothing visible. */
    const uint32_t xorBpp = 24;
    uint32_t xorRowBytes = w * 3u;
    xorRowBytes = (xorRowBytes + 1u) & ~1u;          /* pad to 2 bytes (WORD) */
    uint32_t xorLen = xorRowBytes * h;

    /* ── Build the AND (transparency) mask ────────────────────────────────
     * 1bpp, BOTTOM-UP, each scanline padded to a 2-byte boundary. A SET bit
     * means "transparent" (client shows the underlying pixel). Derive it from
     * alpha: a (near-)transparent pixel -> transparent (bit 1), else opaque
     * (bit 0). We treat alpha < 128 as transparent so the dark fringe of
     * premultiplied anti-aliased edges is masked out rather than painted. */
    uint32_t andRowBytes = ((w + 7u) / 8u);
    andRowBytes = (andRowBytes + 1u) & ~1u;          /* pad to 2 bytes */
    uint32_t andLen = andRowBytes * h;

    /* calloc so the WORD-padding bytes at the end of each xor row stay 0. */
    uint8_t *xorData = (uint8_t *)calloc(1, xorLen);
    uint8_t *andData = (uint8_t *)calloc(1, andLen); /* default opaque (0) */
    if (!xorData || !andData) { free(xorData); free(andData); return; }

    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *srcRow = bgra + (size_t)y * (w * 4u);
        /* bottom-up: source row y goes to dest row (h-1-y). */
        uint8_t *dstRow = xorData + (size_t)(h - 1 - y) * xorRowBytes;
        uint8_t *andRow = andData + (size_t)(h - 1 - y) * andRowBytes;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *px  = srcRow + (size_t)x * 4;   /* B,G,R,A */
            uint8_t       *dpx = dstRow + (size_t)x * 3;   /* B,G,R   */
            dpx[0] = px[0];
            dpx[1] = px[1];
            dpx[2] = px[2];
            if (px[3] < 128) {   /* BGRA -> A at +3; <128 == (near-)transparent */
                /* MSB-first bit order within each byte. */
                andRow[x / 8] |= (uint8_t)(0x80u >> (x % 8));
            }
        }
    }

    UINT rc = CHANNEL_RC_OK;
    bool ok = false;

    pthread_mutex_lock(&ctx->xportLock);
    if (w <= RDP_CURSOR_NEW_MAX && h <= RDP_CURSOR_NEW_MAX) {
        if (pointer->PointerNew) {
            POINTER_NEW_UPDATE upd = {0};
            upd.xorBpp = (UINT16)xorBpp;
            upd.colorPtrAttr.cacheIndex    = 0;
            upd.colorPtrAttr.hotSpotX      = hotX;
            upd.colorPtrAttr.hotSpotY      = hotY;
            upd.colorPtrAttr.width         = (UINT16)w;
            upd.colorPtrAttr.height        = (UINT16)h;
            upd.colorPtrAttr.lengthAndMask = andLen;
            upd.colorPtrAttr.lengthXorMask = xorLen;
            upd.colorPtrAttr.xorMaskData   = xorData;
            upd.colorPtrAttr.andMaskData   = andData;
            ok = pointer->PointerNew(peer->context, &upd) ? true : false;
        }
    } else if (pointer->PointerLarge) {
        POINTER_LARGE_UPDATE upd = {0};
        upd.xorBpp        = (UINT16)xorBpp;
        upd.cacheIndex    = 0;
        upd.hotSpotX      = hotX;
        upd.hotSpotY      = hotY;
        upd.width         = (UINT16)w;
        upd.height        = (UINT16)h;
        upd.lengthAndMask = andLen;
        upd.lengthXorMask = xorLen;
        upd.xorMaskData   = xorData;
        upd.andMaskData   = andData;
        ok = pointer->PointerLarge(peer->context, &upd) ? true : false;
    }
    pthread_mutex_unlock(&ctx->xportLock);

    free(xorData);
    free(andData);

    if (!ok) rdp_error("cursor send failed (rc=%u, %ux%u)", rc, w, h);
    else     rdp_debug("sent cursor %ux%u hot=(%u,%u) xorBpp=%u xor=%u and=%u",
                       w, h, hotX, hotY, xorBpp, xorLen, andLen);
}

bool rdp_peer_send_bitmap(freerdp_peer *peer,
                           const uint8_t *bgra, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height) {
    rdpUpdate *update = peer->context->update;
    SURFACE_BITS_COMMAND cmd = {0};
    cmd.destLeft             = (UINT16)x;
    cmd.destTop              = (UINT16)y;
    cmd.destRight            = (UINT16)(x + width);
    cmd.destBottom           = (UINT16)(y + height);
    cmd.bmp.bpp              = 32;
    cmd.bmp.width            = (UINT16)width;
    cmd.bmp.height           = (UINT16)height;
    cmd.bmp.bitmapData       = (BYTE *)bgra;
    cmd.bmp.bitmapDataLength = width * height * 4;
    cmd.bmp.codecID          = RDP_CODEC_ID_NONE;
    return update->SurfaceBits(update->context, &cmd);
}

bool rdp_peer_send_audio(freerdp_peer *peer,
                          const int16_t *samples, uint32_t frame_count) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->rdpsnd || !ctx->audioReady) return false;
    /* SendSamples(context, buf, nframes, timestamp) — real 3.x signature.
     * Audio runs on its own capture thread; serialize against the transport. */
    pthread_mutex_lock(&ctx->xportLock);
    UINT rc = ctx->rdpsnd->SendSamples(ctx->rdpsnd, samples, frame_count, 0);
    pthread_mutex_unlock(&ctx->xportLock);
    return rc == CHANNEL_RC_OK;
}

bool rdp_peer_send_clipboard(freerdp_peer *peer,
                              const uint8_t *data, size_t len,
                              uint32_t format) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->cliprdr) { rdp_verbose("no clipboard channel"); return false; }

    /* Store an owned copy of the host clipboard; we hand it to the client only
     * when it sends a Format Data Request. Advertising data unsolicited (the
     * old behaviour) violates MS-RDPECLIP and clients ignore it. */
    free(ctx->clipData);
    ctx->clipData = (uint8_t *)malloc(len);
    if (!ctx->clipData) { ctx->clipLen = 0; return false; }
    memcpy(ctx->clipData, data, len);
    ctx->clipLen    = len;
    ctx->clipFormat = format;

    /* Advertise the available format; the client requests the bytes on paste.
     *
     * msgType MUST be CB_FORMAT_LIST. Leaving it 0 (CB_TYPE_NONE) triggers the
     * FreeRDP warning "cliprdr_packet_format_list_new: called with invalid type
     * 00000000". And a Format List is NOT a response — msgFlags must be 0, never
     * CB_RESPONSE_OK (that flag belongs on *_RESPONSE PDUs).
     *
     * formatName MUST be a non-NULL (empty) string, NOT NULL. With
     * useLongFormatNames=TRUE the FreeRDP serializer miscomputes the PDU length for
     * a NULL name and under-allocates the send stream, then aborts on a
     * Stream_Write_UINT32 (WinPR assert Stream_GetRemainingCapacity>=4) — a hard
     * crash that took down the whole daemon mid-clipboard-exchange. An empty ""
     * yields a 2-byte UTF-16 null terminator and a consistent length. Standard
     * formats (CF_UNICODETEXT etc.) are still keyed by id alone. */
    CLIPRDR_FORMAT fmt   = { .formatId = (UINT32)format, .formatName = (char *)"" };
    CLIPRDR_FORMAT_LIST list = {0};
    list.common.msgType  = CB_FORMAT_LIST;
    list.common.msgFlags = 0;
    list.numFormats      = 1;
    list.formats         = &fmt;
    /* Called from the clipboard poll thread; serialize against the transport. */
    pthread_mutex_lock(&ctx->xportLock);
    ctx->cliprdr->ServerFormatList(ctx->cliprdr, &list);
    pthread_mutex_unlock(&ctx->xportLock);
    rdp_verbose("clipboard: advertised format 0x%08x (%zu bytes held)", format, len);
    return true;
}
