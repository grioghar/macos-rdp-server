#define RDP_LOG_COMPONENT "peer"
#include "logging/RDPLog.h"
#include "protocol/RDPPeer.h"
#include "protocol/RDPWebDAV.h"
#include "audio/AudioInput.h"

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
#include <winpr/stream.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* ── MS-RDPEFS (rdpdr) protocol constants ──────────────────────────────── */
/* Packet ids from MS-RDPEFS specification §2.2.1.1 */
#define RDPDR_CTYP_CORE                  0x4472
#define PAKID_CORE_SERVER_ANNOUNCE       0x496E
#define PAKID_CORE_CLIENTID_CONFIRM      0x4343
#define PAKID_CORE_CLIENT_NAME           0x434E
#define PAKID_CORE_CAPABILITY_REQUEST    0x5350
#define PAKID_CORE_CAPABILITY_RESPONSE   0x4350
#define PAKID_CORE_CLIENT_ANNOUNCE_REPLY 0x4352
#define PAKID_CORE_DEVICE_LIST_ANNOUNCE  0x4441
#define PAKID_CORE_DEVICE_REPLY          0x6472
#define PAKID_CORE_DEVICE_IOCOMPLETION   0x4943   /* IRP I/O completion from client */
#define RDPDR_DTYP_FILESYSTEM            0x00000008
#define RDPDR_DTYP_PRINT                 0x00000004
#define RDPDR_DTYP_SERIAL                0x00000001
#define RDPDR_DTYP_PARALLEL              0x00000002
#define RDPDR_DTYP_SMARTCARD             0x00000020
#define CAP_GENERAL_TYPE                 0x0001
#define RDPDR_VERSION_MAJOR              0x0001
#define RDPDR_VERSION_MINOR              0x000C

/* IRP major function codes (MS-RDPEFS §2.2.1.4) — guard against WinPR
 * redefinition (winpr/ntdef.h or similar may define these). */
#ifndef IRP_MJ_CREATE
#define IRP_MJ_CREATE                    0x00000000
#endif
#ifndef IRP_MJ_CLOSE
#define IRP_MJ_CLOSE                     0x00000002
#endif
#ifndef IRP_MJ_READ
#define IRP_MJ_READ                      0x00000003
#endif
#ifndef IRP_MJ_WRITE
#define IRP_MJ_WRITE                     0x00000004
#endif
#ifndef IRP_MJ_QUERY_INFORMATION
#define IRP_MJ_QUERY_INFORMATION         0x00000005
#endif
#ifndef IRP_MJ_SET_INFORMATION
#define IRP_MJ_SET_INFORMATION           0x00000006
#endif
#ifndef IRP_MJ_DIRECTORY_CONTROL
#define IRP_MJ_DIRECTORY_CONTROL         0x0000000C
#endif

/* IRP minor functions for IRP_MJ_DIRECTORY_CONTROL */
#define IRP_MN_QUERY_DIRECTORY           0x00000001

/* PAKID_CORE_DEVICE_IOREQUEST — client-to-server IRP request packet */
#define PAKID_CORE_DEVICE_IOREQUEST      0x4952

/* IRP_MJ_QUERY_INFORMATION / IRP_MJ_SET_INFORMATION classes */
#define RDPDR_FileBasicInformation       0x00000004  /* timestamps + attrs */
#define RDPDR_FileStandardInformation    0x00000005  /* size + allocation */
#define RDPDR_FileDispositionInformation 0x0000000D  /* mark for deletion */
#define RDPDR_FileFullDirectoryInformation 0x00000002 /* directory listing */

/* NTSTATUS codes relevant to RDPDR — prefixed to avoid redefinition if WinPR
 * defines the generic STATUS_SUCCESS in its ntstatus.h. */
#define RDPDR_STATUS_SUCCESS             0x00000000
#define RDPDR_STATUS_NO_MORE_FILES       0x80000006  /* end of directory listing */

/* IRP_MJ_CREATE access and disposition constants */
#define RDPDR_GENERIC_READ               0x80000000
#define RDPDR_GENERIC_WRITE              0x40000000
#define RDPDR_FILE_OPEN                  0x00000001
#define RDPDR_FILE_CREATE                0x00000002
#define RDPDR_FILE_OPEN_IF               0x00000003
#define RDPDR_FILE_OVERWRITE_IF          0x00000005
#define RDPDR_FILE_DIRECTORY_FILE        0x00000001
#define RDPDR_FILE_NON_DIRECTORY_FILE    0x00000040
#define RDPDR_DELETE_ON_CLOSE            0x00001000

/* Handshake state for the rdpdr channel */
typedef enum {
    kRdpdrIdle = 0,
    kRdpdrSentAnnounce,
    kRdpdrReceivedName,
    kRdpdrReady,
    kRdpdrError,
} RdpdrHandshakeState;

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
    /* Unmount and destroy WebDAV servers first — they hold a pointer to the peer
     * which must still be valid when we call destroy. */
    for (int i = 0; i < RDPDR_MAX_DEVICES; i++) {
        if (c->webdavServers[i]) {
            rdp_webdav_unmount(c->webdavServers[i]);
            rdp_webdav_server_destroy(c->webdavServers[i]);
            c->webdavServers[i] = NULL;
        }
    }
    if (c->gfx)    { rdpgfx_server_context_free(c->gfx);    c->gfx    = NULL; }
    if (c->cliprdr){ cliprdr_server_context_free(c->cliprdr);c->cliprdr= NULL; }
    if (c->rdpsnd) { rdpsnd_server_context_free(c->rdpsnd);  c->rdpsnd = NULL; }
    /* Audio input (MS-RDPEAI) — close and release the ObjC object. */
    if (c->audioInput) { rdp_audio_input_close(c->audioInput); c->audioInput = NULL; }
    /* Close rdpdr raw WTS channel (opened when RDP_RDPDR_ENABLED=1). */
    if (c->rdpdrChannel && c->rdpdrChannel != INVALID_HANDLE_VALUE) {
        WTSVirtualChannelClose(c->rdpdrChannel);
        c->rdpdrChannel = NULL;
        c->rdpdrEvent   = NULL;
    }
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
    RDPPeerContext *ctx = (RDPPeerContext *)cliprdr->custom;
    /* Client engaged the channel — it is now safe to advertise (ServerFormatList). */
    if (ctx) ctx->clipReady = true;
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
    ctx->clipReady = true;   /* channel fully engaged — Mac->Win advertise is safe */
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
    /* dataLen MUST be 4: FreeRDP's cliprdr_server_format_data_request allocates a
     * stream of (common.dataLen + 8) and then writes the 4-byte requestedFormatId
     * AFTER the 8-byte header. With dataLen=0 the stream is exactly the header and
     * the formatId write runs off the end -> WinPR Stream_Write_UINT32 abort (the
     * crash that locked the client out on every connect). */
    dreq.common.dataLen   = 4;
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

    /* RDPDR drive-redirection static VC. Gated behind RDP_RDPDR_ENABLED=1. The
     * channel open is best-effort: if the client didn't advertise "rdpdr" in its
     * channel list, WTSVirtualChannelOpen returns NULL and we log + continue. */
    rdp_peer_open_rdpdr(peer);

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

    /* Audio input (MS-RDPEAI mic redirection). Gated behind RDP_AUDIO_INPUT=1.
     * Best-effort: if the channel is not available (client did not advertise
     * mic redirection), openForPeer returns NULL and we continue silently. */
    ctx->audioInput = rdp_audio_input_open(peer);
    if (ctx->audioInput)
        rdp_info("audio_input: channel open");

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

    HANDLE events[70] = {0};   /* transport(64) + vcm + gfx + clip + rdpdr + ai_input + spare */
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

    /* RDPDR static VC event (only when RDP_RDPDR_ENABLED=1 and channel open). */
    if (ctx->rdpdrEvent) events[nCount++] = ctx->rdpdrEvent;

    /* AUDIO_INPUT channel event (only when RDP_AUDIO_INPUT=1 and channel open). */
    HANDLE aiEvent = rdp_audio_input_event(ctx->audioInput);
    if (aiEvent) events[nCount++] = aiEvent;

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

    /* Drain rdpdr static VC messages (RDP_RDPDR_ENABLED=1 only). */
    if (ok && ctx->rdpdrEvent &&
        WaitForSingleObject(ctx->rdpdrEvent, 0) == WAIT_OBJECT_0) {
        rdp_peer_pump_rdpdr(peer);
    }

    /* Drain AUDIO_INPUT DATA PDUs and play on Mac speaker (RDP_AUDIO_INPUT=1).
     * rdp_audio_input_pump is a read-only drain — no transport writes — so it
     * does not strictly need xportLock, but we hold it here anyway for a
     * consistent single-threaded pump discipline (mirrors rdpdr). */
    if (ok && ctx->audioInput) {
        if (!aiEvent || WaitForSingleObject(aiEvent, 0) == WAIT_OBJECT_0) {
            rdp_audio_input_pump(ctx->audioInput);
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
     * old behaviour) violates MS-RDPECLIP and clients ignore it.
     *
     * Thread safety: malloc+copy happens outside the lock (no heap allocation
     * while holding xportLock keeps the audio thread unblocked). We then
     * take xportLock for the pointer swap, which is the same lock the reader
     * (cliprdr_client_format_data_request) holds while accessing clipData/Len.
     * This prevents a reader from seeing a half-updated clipData/clipLen pair. */
    uint8_t *newClipData = (uint8_t *)malloc(len ? len : 1);
    if (!newClipData) { rdp_verbose("clipboard: malloc failed"); return false; }
    if (len) memcpy(newClipData, data, len);
    uint8_t *oldClipData;
    pthread_mutex_lock(&ctx->xportLock);
    oldClipData      = ctx->clipData;
    ctx->clipData    = newClipData;
    ctx->clipLen     = len;
    ctx->clipFormat  = format;
    pthread_mutex_unlock(&ctx->xportLock);
    free(oldClipData);  /* free old buffer AFTER lock released */

    /* Hold the data but DO NOT advertise until the cliprdr handshake is complete.
     * Calling ServerFormatList before the client has engaged the channel corrupts
     * FreeRDP's cliprdr send stream and aborts the daemon (the crash-loop that gave
     * the client 0x904). Once ready, the next pasteboard change re-advertises. */
    if (!ctx->clipReady) {
        rdp_verbose("clipboard: held %zu bytes (fmt 0x%08x) — channel not ready, "
                    "advertise deferred", len, format);
        return true;
    }


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

/* ── RDPDR (MS-RDPEFS) drive-redirection implementation ─────────────────── */

/* Write a complete wStream PDU to the rdpdr WTS channel.
 * Called under xportLock (transport write). */
static bool rdpdr_write_pdu(HANDLE ch, wStream *s) {
    size_t len     = Stream_GetPosition(s);
    BYTE  *buf     = Stream_Buffer(s);
    ULONG  written = 0;
    BOOL   ok      = WTSVirtualChannelWrite(ch, (PCHAR)buf, (ULONG)len, &written);
    return ok && written == (ULONG)len;
}

static bool rdpdr_send_server_announce(HANDLE ch) {
    /* Header(4) + VersionMajor(2) + VersionMinor(2) + ClientId(4) */
    wStream *s = Stream_New(NULL, 12);
    if (!s) return false;
    Stream_Write_UINT16(s, RDPDR_CTYP_CORE);
    Stream_Write_UINT16(s, PAKID_CORE_SERVER_ANNOUNCE);
    Stream_Write_UINT16(s, RDPDR_VERSION_MAJOR);
    Stream_Write_UINT16(s, RDPDR_VERSION_MINOR);
    Stream_Write_UINT32(s, 1);   /* initial ClientId = 1 */
    bool ok = rdpdr_write_pdu(ch, s);
    Stream_Free(s, TRUE);
    if (ok) rdp_verbose("rdpdr: -> SERVER_ANNOUNCE");
    return ok;
}

static bool rdpdr_send_caps_request(HANDLE ch) {
    /* Header(4) + numCaps(2) + pad(2) + one GeneralCap(44) = 52 bytes */
    wStream *s = Stream_New(NULL, 52);
    if (!s) return false;
    Stream_Write_UINT16(s, RDPDR_CTYP_CORE);
    Stream_Write_UINT16(s, PAKID_CORE_CAPABILITY_REQUEST);
    Stream_Write_UINT16(s, 1);    /* numCapabilities */
    Stream_Write_UINT16(s, 0);    /* padding */
    /* General capability set (MS-RDPEFS §2.2.2.7.1) */
    Stream_Write_UINT16(s, CAP_GENERAL_TYPE);
    Stream_Write_UINT16(s, 44);   /* capabilityLength */
    Stream_Write_UINT32(s, 2);    /* version */
    Stream_Write_UINT32(s, 0);    /* osType */
    Stream_Write_UINT32(s, 0);    /* osVersion */
    Stream_Write_UINT16(s, RDPDR_VERSION_MAJOR);
    Stream_Write_UINT16(s, RDPDR_VERSION_MINOR);
    Stream_Write_UINT32(s, 0x00007fff); /* ioCode1 */
    Stream_Write_UINT32(s, 0);          /* ioCode2 */
    Stream_Write_UINT32(s, 0x0000000f); /* extendedPDU */
    Stream_Write_UINT32(s, 0);          /* extraFlags1 */
    Stream_Write_UINT32(s, 0);          /* extraFlags2 */
    Stream_Write_UINT32(s, 0);          /* specialTypeDeviceCap */
    bool ok = rdpdr_write_pdu(ch, s);
    Stream_Free(s, TRUE);
    if (ok) rdp_verbose("rdpdr: -> CAPABILITY_REQUEST (1 cap)");
    return ok;
}

static bool rdpdr_send_clientid_confirm(HANDLE ch, uint16_t clientId) {
    wStream *s = Stream_New(NULL, 12);
    if (!s) return false;
    Stream_Write_UINT16(s, RDPDR_CTYP_CORE);
    Stream_Write_UINT16(s, PAKID_CORE_CLIENTID_CONFIRM);
    Stream_Write_UINT16(s, RDPDR_VERSION_MAJOR);
    Stream_Write_UINT16(s, RDPDR_VERSION_MINOR);
    Stream_Write_UINT32(s, (UINT32)clientId);
    bool ok = rdpdr_write_pdu(ch, s);
    Stream_Free(s, TRUE);
    if (ok) rdp_verbose("rdpdr: -> CLIENTID_CONFIRM (clientId=%u)", (unsigned)clientId);
    return ok;
}

static void rdpdr_send_device_reply(HANDLE ch, uint32_t deviceId, uint32_t result) {
    wStream *s = Stream_New(NULL, 12);
    if (!s) return;
    Stream_Write_UINT16(s, RDPDR_CTYP_CORE);
    Stream_Write_UINT16(s, PAKID_CORE_DEVICE_REPLY);
    Stream_Write_UINT32(s, deviceId);
    Stream_Write_UINT32(s, result);
    rdpdr_write_pdu(ch, s);
    Stream_Free(s, TRUE);
}

/* Allocate the next monotonic IRP request id and store a pending entry.
 * cb may be NULL for fire-and-forget.  Returns 0 if the table is full. */
static uint32_t rdpdr_alloc_request(RDPPeerContext *ctx,
                                     uint32_t deviceId, const char *path,
                                     RDPIrpCallback cb, void *userdata) {
    for (int i = 0; i < RDPDR_MAX_PENDING; i++) {
        if (ctx->rdpdrPending[i].requestId == 0) {
            uint32_t rid = ++ctx->rdpdrNextReqId;
            if (rid == 0) rid = ++ctx->rdpdrNextReqId; /* skip id 0 (sentinel) */
            ctx->rdpdrPending[i].requestId = rid;
            ctx->rdpdrPending[i].deviceId  = deviceId;
            ctx->rdpdrPending[i].callback  = cb;
            ctx->rdpdrPending[i].userdata  = userdata;
            strncpy(ctx->rdpdrPending[i].path, path ? path : "",
                    sizeof(ctx->rdpdrPending[i].path) - 1);
            ctx->rdpdrPending[i].path[sizeof(ctx->rdpdrPending[i].path) - 1] = '\0';
            return rid;
        }
    }
    rdp_verbose("rdpdr: pending table full — dropping IRP for dev %u", deviceId);
    return 0;
}

/* Free a slot in the pending table by request id and invoke its callback.
 * payload/payloadLen are the bytes following the IOCOMPLETION fixed header.
 * Returns the device id associated with the request, or 0 if not found. */
static uint32_t rdpdr_free_request(RDPPeerContext *ctx, uint32_t requestId,
                                    uint32_t ioStatus,
                                    const uint8_t *payload, uint32_t payloadLen) {
    for (int i = 0; i < RDPDR_MAX_PENDING; i++) {
        if (ctx->rdpdrPending[i].requestId == requestId) {
            uint32_t       devId = ctx->rdpdrPending[i].deviceId;
            RDPIrpCallback cb    = ctx->rdpdrPending[i].callback;
            void          *ud    = ctx->rdpdrPending[i].userdata;
            rdp_verbose("rdpdr: IRP completion for requestId=%u dev=%u path=\"%s\"",
                        requestId, devId, ctx->rdpdrPending[i].path);
            ctx->rdpdrPending[i].requestId = 0;
            ctx->rdpdrPending[i].callback  = NULL;
            ctx->rdpdrPending[i].userdata  = NULL;
            /* Invoke callback AFTER zeroing the slot so re-entrant alloc is safe. */
            if (cb) cb(ioStatus, payload, payloadLen, ud);
            return devId;
        }
    }
    return 0;
}

/*
 * Send a DR_DRIVE_QUERY_INFORMATION_REQ to ask the client for the standard
 * file info (size + allocation) for the drive root. This is the simplest IRP
 * that gives us something useful (free space / total size) without requiring a
 * CREATE first. The server opens fileId=0 (root pseudo-handle) and asks for
 * RDPDR_FileStandardInformation. The client will respond with
 * PAKID_CORE_DEVICE_IOCOMPLETION carrying an IoStatus and the 24-byte
 * FILE_STANDARD_INFORMATION structure.
 *
 * MS-RDPEFS §2.2.3.3.9  DR_DRIVE_QUERY_INFORMATION_REQ
 * MS-RDPEFS §2.2.3.4.9  DR_DRIVE_QUERY_INFORMATION_RSP
 */
static bool rdpdr_send_query_info_req(RDPPeerContext *ctx, uint32_t deviceId) {
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, "\\", NULL, NULL);
    if (rid == 0) return false;

    /* Header(4) + DeviceId(4) + FileId(4) + CompletionId(4) +
     * MajorFunction(4) + MinorFunction(4) + Padding(20) = IRP header 44 bytes
     * + FsInformationClass(4) + Padding(4) = 52 bytes total */
    wStream *s = Stream_New(NULL, 52);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    /* Packet header */
    Stream_Write_UINT16(s, RDPDR_CTYP_CORE);
    Stream_Write_UINT16(s, PAKID_CORE_DEVICE_IOREQUEST);

    /* DR_IRP_REQ — MS-RDPEFS §2.2.1.4 */
    Stream_Write_UINT32(s, deviceId);
    Stream_Write_UINT32(s, 0);          /* FileId = 0 (root) */
    Stream_Write_UINT32(s, rid);        /* CompletionId (our request id) */
    Stream_Write_UINT32(s, IRP_MJ_QUERY_INFORMATION);
    Stream_Write_UINT32(s, 0);          /* MinorFunction = 0 */
    /* 20 bytes of padding to complete the 40-byte IRP header body */
    Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0);

    /* DR_DRIVE_QUERY_INFORMATION_REQ body */
    Stream_Write_UINT32(s, RDPDR_FileStandardInformation);
    Stream_Write_UINT32(s, 0); /* padding */

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (ok)
        rdp_verbose("rdpdr: -> QUERY_INFORMATION_REQ (dev=%u rid=%u "
                    "RDPDR_FileStandardInformation)", deviceId, rid);
    else
        rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    return ok;
}

/* ── IRP send helpers ─────────────────────────────────────────────────────
 *
 * Each helper builds the standard DR_IRP_REQ header (MS-RDPEFS §2.2.1.4):
 *   Packet header  : RDPDR_CTYP_CORE(2) + PAKID_CORE_DEVICE_IOREQUEST(2)
 *   IRP body header: DeviceId(4) + FileId(4) + CompletionId(4) +
 *                    MajorFunction(4) + MinorFunction(4) + Padding(20)
 * Total fixed header = 4 + 40 = 44 bytes before the specific body.
 *
 * ALL helpers must be called under xportLock.
 * cb may be NULL for fire-and-forget requests.
 */

/* Write the common 44-byte IRP header into stream s. */
static void rdpdr_write_irp_header(wStream *s, uint32_t deviceId,
                                    uint32_t fileId, uint32_t rid,
                                    uint32_t majorFn, uint32_t minorFn) {
    Stream_Write_UINT16(s, RDPDR_CTYP_CORE);
    Stream_Write_UINT16(s, PAKID_CORE_DEVICE_IOREQUEST);
    Stream_Write_UINT32(s, deviceId);
    Stream_Write_UINT32(s, fileId);
    Stream_Write_UINT32(s, rid);
    Stream_Write_UINT32(s, majorFn);
    Stream_Write_UINT32(s, minorFn);
    /* 20 bytes of padding (5 x uint32 = 0x00 filler per spec). */
    Stream_Write_UINT32(s, 0); Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0); Stream_Write_UINT32(s, 0);
    Stream_Write_UINT32(s, 0);
}

/* IRP_MJ_CREATE — open a file or directory.
 * path is UTF-8, no leading slash; internally converted to UTF-16LE. */
bool rdpdr_send_create_req(RDPPeerContext *ctx, uint32_t deviceId,
                            const char *path,
                            uint32_t desiredAccess,
                            uint32_t createDisposition,
                            uint32_t createOptions,
                            RDPIrpCallback cb, void *userdata) {
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, path, cb, userdata);
    if (rid == 0) return false;

    /* Convert path to UTF-16LE. */
    size_t pathLen = path ? strlen(path) : 0;
    /* Max path: 260 chars → 520 bytes UTF-16 + 2 for NUL. */
    uint16_t utf16[261];
    uint32_t utf16Len = 0;
    for (size_t i = 0; i < pathLen && i < 260; i++) {
        /* Simple ASCII->UTF-16LE (rdpdr paths are drive-relative ASCII). */
        utf16[utf16Len++] = (uint16_t)(unsigned char)path[i];
    }
    uint32_t pathBytes = utf16Len * 2; /* byte count sent on wire (no NUL) */

    /* DR_CREATE_REQ body (MS-RDPEFS §2.2.3.3.1):
     *   DesiredAccess(4) + AllocationSize(8) + FileAttributes(4) +
     *   ShareAccess(4) + CreateDisposition(4) + CreateOptions(4) +
     *   PathLength(4) + Path(PathLength) */
    size_t totalSize = 44 + 32 + pathBytes;
    wStream *s = Stream_New(NULL, totalSize);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    rdpdr_write_irp_header(s, deviceId, 0, rid, IRP_MJ_CREATE, 0);
    Stream_Write_UINT32(s, desiredAccess);
    Stream_Write_UINT64(s, 0);            /* AllocationSize = 0 */
    Stream_Write_UINT32(s, 0);            /* FileAttributes = normal */
    Stream_Write_UINT32(s, 3);            /* ShareAccess = read|write */
    Stream_Write_UINT32(s, createDisposition);
    Stream_Write_UINT32(s, createOptions);
    Stream_Write_UINT32(s, pathBytes);
    for (uint32_t i = 0; i < utf16Len; i++) Stream_Write_UINT16(s, utf16[i]);

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (!ok) rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    else rdp_verbose("rdpdr: -> CREATE_REQ dev=%u rid=%u path=\"%s\"",
                     deviceId, rid, path ? path : "");
    return ok;
}

/* IRP_MJ_CLOSE — close a file handle. */
bool rdpdr_send_close_req(RDPPeerContext *ctx, uint32_t deviceId,
                           uint32_t fileId,
                           RDPIrpCallback cb, void *userdata) {
    char label[32];
    snprintf(label, sizeof(label), "close(%u)", fileId);
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, label, cb, userdata);
    if (rid == 0) return false;

    /* DR_CLOSE_REQ body: just 32 bytes of padding after the IRP header. */
    wStream *s = Stream_New(NULL, 44 + 32);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    rdpdr_write_irp_header(s, deviceId, fileId, rid, IRP_MJ_CLOSE, 0);
    /* 32 bytes Padding */
    for (int i = 0; i < 8; i++) Stream_Write_UINT32(s, 0);

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (!ok) rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    else rdp_verbose("rdpdr: -> CLOSE_REQ dev=%u fileId=%u rid=%u",
                     deviceId, fileId, rid);
    return ok;
}

/* IRP_MJ_READ — read bytes from an open file. */
bool rdpdr_send_read_req(RDPPeerContext *ctx, uint32_t deviceId,
                          uint32_t fileId, uint64_t offset, uint32_t length,
                          RDPIrpCallback cb, void *userdata) {
    char label[32];
    snprintf(label, sizeof(label), "read(%u)", fileId);
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, label, cb, userdata);
    if (rid == 0) return false;

    /* DR_READ_REQ body (MS-RDPEFS §2.2.3.3.3):
     *   Length(4) + Offset(8) + Padding(20) */
    wStream *s = Stream_New(NULL, 44 + 32);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    rdpdr_write_irp_header(s, deviceId, fileId, rid, IRP_MJ_READ, 0);
    Stream_Write_UINT32(s, length);
    Stream_Write_UINT64(s, offset);
    /* Padding: 20 bytes (5 x uint32) */
    for (int i = 0; i < 5; i++) Stream_Write_UINT32(s, 0);

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (!ok) rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    else rdp_verbose("rdpdr: -> READ_REQ dev=%u fileId=%u off=%llu len=%u rid=%u",
                     deviceId, fileId, (unsigned long long)offset, length, rid);
    return ok;
}

/* IRP_MJ_WRITE — write bytes to an open file. */
bool rdpdr_send_write_req(RDPPeerContext *ctx, uint32_t deviceId,
                           uint32_t fileId, uint64_t offset,
                           const uint8_t *data, uint32_t length,
                           RDPIrpCallback cb, void *userdata) {
    char label[32];
    snprintf(label, sizeof(label), "write(%u)", fileId);
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, label, cb, userdata);
    if (rid == 0) return false;

    /* DR_WRITE_REQ body (MS-RDPEFS §2.2.3.3.4):
     *   Length(4) + Offset(8) + Padding(20) + WriteData(Length) */
    wStream *s = Stream_New(NULL, 44 + 32 + length);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    rdpdr_write_irp_header(s, deviceId, fileId, rid, IRP_MJ_WRITE, 0);
    Stream_Write_UINT32(s, length);
    Stream_Write_UINT64(s, offset);
    for (int i = 0; i < 5; i++) Stream_Write_UINT32(s, 0); /* padding */
    if (length > 0 && data) Stream_Write(s, data, length);

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (!ok) rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    else rdp_verbose("rdpdr: -> WRITE_REQ dev=%u fileId=%u off=%llu len=%u rid=%u",
                     deviceId, fileId, (unsigned long long)offset, length, rid);
    return ok;
}

/* IRP_MJ_DIRECTORY_CONTROL / IRP_MN_QUERY_DIRECTORY — list a directory.
 * pattern is the search pattern (e.g. "*"); informationClass is
 * FileFullDirectoryInformation (2). */
bool rdpdr_send_query_dir_req(RDPPeerContext *ctx, uint32_t deviceId,
                               uint32_t fileId, const char *pattern,
                               RDPIrpCallback cb, void *userdata) {
    char label[64];
    snprintf(label, sizeof(label), "querydir(%u,%s)", fileId,
             pattern ? pattern : "*");
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, label, cb, userdata);
    if (rid == 0) return false;

    /* Convert pattern to UTF-16LE */
    const char *pat = pattern ? pattern : "*";
    size_t patLen = strlen(pat);
    uint16_t utf16[261];
    uint32_t utf16Len = 0;
    for (size_t i = 0; i < patLen && i < 260; i++)
        utf16[utf16Len++] = (uint16_t)(unsigned char)pat[i];
    uint32_t patBytes = utf16Len * 2;

    /* DR_DRIVE_QUERY_DIRECTORY_REQ (MS-RDPEFS §2.2.3.3.10):
     *   FsInformationClass(4) + InitialQuery(1) + PathLength(4) + Padding(23) + Path */
    size_t bodySize = 4 + 1 + 4 + 23 + patBytes;
    wStream *s = Stream_New(NULL, 44 + bodySize);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    rdpdr_write_irp_header(s, deviceId, fileId, rid,
                            IRP_MJ_DIRECTORY_CONTROL, IRP_MN_QUERY_DIRECTORY);
    Stream_Write_UINT32(s, RDPDR_FileFullDirectoryInformation);
    Stream_Write_UINT8(s,  1);           /* InitialQuery = 1 (restart scan) */
    Stream_Write_UINT32(s, patBytes);
    /* 23 bytes padding */
    for (int i = 0; i < 5; i++) Stream_Write_UINT32(s, 0);
    Stream_Write_UINT8(s, 0);            /* last padding byte */
    /* Path */
    for (uint32_t i = 0; i < utf16Len; i++) Stream_Write_UINT16(s, utf16[i]);

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (!ok) rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    else rdp_verbose("rdpdr: -> QUERY_DIR_REQ dev=%u fileId=%u pat=\"%s\" rid=%u",
                     deviceId, fileId, pat, rid);
    return ok;
}

/* IRP_MJ_SET_INFORMATION (FileDispositionInformation) — mark file for deletion. */
bool rdpdr_send_delete_req(RDPPeerContext *ctx, uint32_t deviceId,
                            uint32_t fileId,
                            RDPIrpCallback cb, void *userdata) {
    char label[32];
    snprintf(label, sizeof(label), "delete(%u)", fileId);
    uint32_t rid = rdpdr_alloc_request(ctx, deviceId, label, cb, userdata);
    if (rid == 0) return false;

    /* DR_SET_INFORMATION_REQ body (MS-RDPEFS §2.2.3.3.9):
     *   FsInformationClass(4) + Length(4) + Padding(24) + SetBuffer(Length)
     * FileDispositionInformation body: DeleteFile(1) = 1. */
    wStream *s = Stream_New(NULL, 44 + 4 + 4 + 24 + 1);
    if (!s) { rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0); return false; }

    rdpdr_write_irp_header(s, deviceId, fileId, rid, IRP_MJ_SET_INFORMATION, 0);
    Stream_Write_UINT32(s, RDPDR_FileDispositionInformation);
    Stream_Write_UINT32(s, 1);   /* Length of SetBuffer = 1 byte */
    for (int i = 0; i < 6; i++) Stream_Write_UINT32(s, 0); /* 24 bytes padding */
    Stream_Write_UINT8(s,  1);   /* DeleteFile = TRUE */

    bool ok = rdpdr_write_pdu(ctx->rdpdrChannel, s);
    Stream_Free(s, TRUE);
    if (!ok) rdpdr_free_request(ctx, rid, 0xC0000000, NULL, 0);
    else rdp_verbose("rdpdr: -> DELETE_REQ dev=%u fileId=%u rid=%u",
                     deviceId, fileId, rid);
    return ok;
}

/* Decode one PDU from the rdpdr channel and advance the state machine. */
static void rdpdr_handle_pdu(RDPPeerContext *ctx, BYTE *buf, ULONG len) {
    wStream stack; /* stack-allocated reader; avoids heap alloc for each PDU */
    wStream *s = Stream_StaticInit(&stack, buf, (size_t)len);

    if (Stream_GetRemainingLength(s) < 4) return;
    UINT16 component, packetId;
    Stream_Read_UINT16(s, component);
    Stream_Read_UINT16(s, packetId);
    if (component != RDPDR_CTYP_CORE) {
        rdp_verbose("rdpdr: unknown component 0x%04x", component);
        return;
    }

    switch (packetId) {

    case PAKID_CORE_CLIENT_ANNOUNCE_REPLY: {
        if (Stream_GetRemainingLength(s) < 8) break;
        UINT16 maj, min; UINT32 cid;
        Stream_Read_UINT16(s, maj);
        Stream_Read_UINT16(s, min);
        Stream_Read_UINT32(s, cid);
        ctx->rdpdrClientId = (uint16_t)(cid & 0xFFFF);
        rdp_verbose("rdpdr: <- CLIENT_ANNOUNCE_REPLY v%u.%u clientId=%u",
                    (unsigned)maj, (unsigned)min, (unsigned)ctx->rdpdrClientId);
        break;
    }

    case PAKID_CORE_CLIENT_NAME: {
        if (Stream_GetRemainingLength(s) < 12) break;
        UINT32 unicodeFlag, codePage, nameLen;
        Stream_Read_UINT32(s, unicodeFlag);
        Stream_Read_UINT32(s, codePage);
        Stream_Read_UINT32(s, nameLen);
        (void)codePage;
        char name[128] = "(empty)";
        if (nameLen > 0 && (size_t)nameLen <= Stream_GetRemainingLength(s)) {
            if (unicodeFlag && nameLen >= 2) {
                size_t chars = (nameLen / 2 < 127) ? nameLen / 2 : 127;
                for (size_t i = 0; i < chars; i++) {
                    UINT16 wc; Stream_Read_UINT16(s, wc);
                    name[i] = (wc && wc < 128) ? (char)wc : '?';
                    if (!wc) { name[i] = '\0'; break; }
                }
                name[chars] = '\0';
            } else {
                size_t n = (nameLen < 127) ? nameLen : 127;
                Stream_Read(s, name, n);
                name[n] = '\0';
            }
        }
        rdp_verbose("rdpdr: <- CLIENT_NAME \"%s\"", name);

        /* Both ANNOUNCE_REPLY and NAME received — send caps + confirm. */
        if (ctx->rdpdrState == kRdpdrSentAnnounce) {
            if (rdpdr_send_caps_request(ctx->rdpdrChannel) &&
                rdpdr_send_clientid_confirm(ctx->rdpdrChannel, ctx->rdpdrClientId)) {
                ctx->rdpdrState = kRdpdrReceivedName;
            } else {
                ctx->rdpdrState = kRdpdrError;
                rdp_error("rdpdr: failed to send caps/confirm");
            }
        }
        break;
    }

    case PAKID_CORE_CAPABILITY_RESPONSE: {
        if (Stream_GetRemainingLength(s) < 4) break;
        UINT16 numCaps, pad;
        Stream_Read_UINT16(s, numCaps);
        Stream_Read_UINT16(s, pad);
        (void)pad;
        rdp_verbose("rdpdr: <- CAPABILITY_RESPONSE (%u caps)", (unsigned)numCaps);
        /* We accept whatever the client advertises; no negotiation needed for
         * enumeration-only mode. Stay in ReceivedName until device list arrives. */
        break;
    }

    case PAKID_CORE_DEVICE_LIST_ANNOUNCE: {
        if (Stream_GetRemainingLength(s) < 4) break;
        UINT32 deviceCount;
        Stream_Read_UINT32(s, deviceCount);
        rdp_info("rdpdr: <- DEVICE_LIST_ANNOUNCE — %u device(s)", (unsigned)deviceCount);

        /* Track how many drive slots we have filled so we can index webdavServers. */
        int driveSlot = 0;

        for (UINT32 i = 0; i < deviceCount; i++) {
            if (Stream_GetRemainingLength(s) < 20) break;
            UINT32 devType, devId, dataLen;
            char dosName[9] = {0};
            Stream_Read_UINT32(s, devType);
            Stream_Read_UINT32(s, devId);
            Stream_Read(s, dosName, 8);
            dosName[8] = '\0';
            Stream_Read_UINT32(s, dataLen);
            if (dataLen > 0 && (size_t)dataLen <= Stream_GetRemainingLength(s))
                Stream_Seek(s, (size_t)dataLen);

            const char *typeName = "unknown";
            switch (devType) {
                case RDPDR_DTYP_FILESYSTEM: typeName = "drive";     break;
                case RDPDR_DTYP_PRINT:      typeName = "printer";   break;
                case RDPDR_DTYP_SERIAL:     typeName = "serial";    break;
                case RDPDR_DTYP_PARALLEL:   typeName = "parallel";  break;
                case RDPDR_DTYP_SMARTCARD:  typeName = "smartcard"; break;
            }

            if (devType == RDPDR_DTYP_FILESYSTEM) {
                rdp_info("rdpdr: client drive #%u — id=%u name=\"%s\" "
                         "(starting embedded WebDAV server for Finder mount)",
                         (unsigned)i, (unsigned)devId, dosName);

                /* Desktop placeholder for immediate user feedback. */
                rdp_drive_mount_placeholder(dosName, devId);

                /* Start an embedded WebDAV server and mount via mount_webdav. */
                if (driveSlot < RDPDR_MAX_DEVICES) {
                    /* Ports 8760..8767 — one per drive slot. */
                    uint16_t port = (uint16_t)(8760 + (driveSlot % 100));
                    /* Must release xportLock before calling server_create which
                     * calls pthread_create and may need to call back into IRPs.
                     * ctx->base.peer is the owning freerdp_peer pointer. */
                    freerdp_peer *peerPtr = ctx->base.peer;
                    pthread_mutex_unlock(&ctx->xportLock);
                    RDPWebDAVServer *srv =
                        rdp_webdav_server_create(peerPtr, devId, dosName, port);
                    pthread_mutex_lock(&ctx->xportLock);

                    if (srv) {
                        ctx->webdavServers[driveSlot] = srv;
                        rdp_webdav_mount(srv);
                        rdp_info("rdpdr: WebDAV server started on port %u for drive \"%s\"",
                                 (unsigned)port, dosName);
                    } else {
                        rdp_error("rdpdr: failed to start WebDAV server for drive \"%s\"",
                                  dosName);
                    }
                    driveSlot++;
                } else {
                    rdp_verbose("rdpdr: no free WebDAV server slots for drive \"%s\"",
                                dosName);
                }

                /* Fire-and-forget probe to log drive capacity. */
                rdpdr_send_query_info_req(ctx, devId);
            } else {
                rdp_verbose("rdpdr: client device #%u — id=%u name=\"%s\" type=%s (not a drive)",
                            (unsigned)i, (unsigned)devId, dosName, typeName);
            }

            /* ACK every device with RDPDR_STATUS_SUCCESS so the client knows we saw it. */
            rdpdr_send_device_reply(ctx->rdpdrChannel, devId, RDPDR_STATUS_SUCCESS);
        }

        ctx->rdpdrState = kRdpdrReady;
        rdp_info("rdpdr: device enumeration complete — handshake done");
        break;
    }

    /* ── IRP I/O Completion (client -> server) ───────────────────────────── */
    case PAKID_CORE_DEVICE_IOCOMPLETION: {
        /* MS-RDPEFS §2.2.1.5  DR_DEVICE_IOCOMPLETION
         * Header(4 already consumed) + DeviceId(4) + CompletionId(4) +
         * IoStatus(4) = 12 more bytes before the payload. */
        if (Stream_GetRemainingLength(s) < 12) {
            rdp_verbose("rdpdr: IOCOMPLETION too short (%zu bytes remaining)",
                        Stream_GetRemainingLength(s));
            break;
        }
        UINT32 devId, completionId, ioStatus;
        Stream_Read_UINT32(s, devId);
        Stream_Read_UINT32(s, completionId);
        Stream_Read_UINT32(s, ioStatus);

        /* Grab the payload bytes before we call rdpdr_free_request (which invokes
         * the callback that might use them).  The stream is stack-allocated over
         * the original read buffer, so the pointer is valid during this call. */
        const uint8_t *payload    = Stream_Pointer(s);
        uint32_t       payloadLen = (uint32_t)Stream_GetRemainingLength(s);

        /* Look up the pending request, invoke its callback, and free the slot. */
        uint32_t matchedDev = rdpdr_free_request(ctx, completionId,
                                                  ioStatus, payload, payloadLen);
        if (matchedDev == 0) {
            /* Could be an unsolicited completion for a request we didn't send
             * (e.g. the client proactively sending info). Log and ignore. */
            rdp_verbose("rdpdr: <- IOCOMPLETION dev=%u cid=%u status=0x%08x "
                        "(no matching pending request)",
                        devId, completionId, ioStatus);
            break;
        }

        rdp_verbose("rdpdr: IOCOMPLETION dev=%u cid=%u status=0x%08x payload=%u bytes",
                    devId, completionId, ioStatus, payloadLen);
        break;
    }

    default:
        rdp_verbose("rdpdr: unhandled packetId=0x%04x in state=%d", packetId, ctx->rdpdrState);
        break;
    }
}

/*
 * Open the "rdpdr" static virtual channel. Called from peer_post_connect when
 * RDP_RDPDR_ENABLED=1. Returns true if the channel opened and SERVER_ANNOUNCE
 * was sent successfully.
 */
bool rdp_peer_open_rdpdr(freerdp_peer *peer) {
    const char *env = getenv("RDP_RDPDR_ENABLED");
    if (!env || strcmp(env, "1") != 0) {
        rdp_verbose("rdpdr: disabled (RDP_RDPDR_ENABLED != 1)");
        return false;
    }

    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->vcm || ctx->vcm == INVALID_HANDLE_VALUE) {
        rdp_error("rdpdr: VCM not open");
        return false;
    }

    /* Open as a STATIC virtual channel. rdpdr is always static (not drdynvc). */
    ctx->rdpdrChannel = WTSVirtualChannelOpen(ctx->vcm,
                                               WTS_CURRENT_SESSION,
                                               (LPSTR)"rdpdr");
    if (!ctx->rdpdrChannel || ctx->rdpdrChannel == INVALID_HANDLE_VALUE) {
        ctx->rdpdrChannel = NULL;
        rdp_verbose("rdpdr: WTSVirtualChannelOpen failed — "
                    "client may not have advertised the channel");
        return false;
    }

    /* Retrieve the channel's event handle for the run-loop WaitForMultipleObjects. */
    void   *evPtr = NULL;
    ULONG   evLen = sizeof(evPtr);
    if (WTSVirtualChannelQuery(ctx->rdpdrChannel,
                               WTSVirtualEventHandle, &evPtr, &evLen) && evPtr) {
        ctx->rdpdrEvent = (HANDLE)*(void **)evPtr;
        WTSFreeMemory(evPtr);
    }

    ctx->rdpdrState    = kRdpdrSentAnnounce;
    ctx->rdpdrClientId = 0;

    /* Send the first handshake PDU under xportLock. */
    pthread_mutex_lock(&ctx->xportLock);
    bool ok = rdpdr_send_server_announce(ctx->rdpdrChannel);
    pthread_mutex_unlock(&ctx->xportLock);

    if (!ok) {
        rdp_error("rdpdr: SERVER_ANNOUNCE write failed");
        WTSVirtualChannelClose(ctx->rdpdrChannel);
        ctx->rdpdrChannel = NULL;
        ctx->rdpdrEvent   = NULL;
        ctx->rdpdrState   = kRdpdrError;
        return false;
    }

    rdp_info("rdpdr: channel open and SERVER_ANNOUNCE sent");
    return true;
}

/* Maximum PDU size for a static virtual channel. The RDP spec limits static
 * VC PDUs to CHANNEL_CHUNK_LENGTH (1600) per write, and rdpdr PDUs are
 * small (handshake packets < 100 bytes; device list < 512 bytes). 4096
 * gives comfortable headroom for a list of many devices. */
#define RDPDR_READ_BUF_SIZE 4096

/*
 * Drain all pending inbound PDUs from the rdpdr channel and advance the
 * MS-RDPEFS handshake state machine. Called from the peer run loop under
 * xportLock whenever the rdpdr event handle fires.
 */
void rdp_peer_pump_rdpdr(freerdp_peer *peer) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->rdpdrChannel || ctx->rdpdrState == kRdpdrError) return;

    /* WTSVirtualChannelRead(handle, timeout_ms, buf, bufSize, &bytesRead).
     * timeout=0 → non-blocking; returns FALSE when no data is pending. */
    BYTE  buf[RDPDR_READ_BUF_SIZE];
    ULONG bytesRead = 0;
    while (WTSVirtualChannelRead(ctx->rdpdrChannel, 0,
                                  (PCHAR)buf, (ULONG)sizeof(buf), &bytesRead)
           && bytesRead > 0) {
        rdpdr_handle_pdu(ctx, buf, bytesRead);
        bytesRead = 0;
    }
}
