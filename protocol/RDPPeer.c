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

static UINT cliprdr_client_format_list(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_FORMAT_LIST *list) {
    rdp_verbose("clipboard: client advertised %u formats", list->numFormats);
    CLIPRDR_FORMAT_LIST_RESPONSE resp = {0};
    resp.common.msgFlags = CB_RESPONSE_OK;
    cliprdr->ServerFormatListResponse(cliprdr, &resp);
    return CHANNEL_RC_OK;
}

static UINT cliprdr_client_format_data(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_FORMAT_DATA_RESPONSE *resp) {
    RDPPeerContext *ctx = (RDPPeerContext *)cliprdr->custom;
    if ((resp->common.msgFlags & CB_RESPONSE_OK) && ctx->callbacks.onClipboard) {
        rdp_verbose("clipboard: %u bytes from client", resp->common.dataLen);
        ctx->callbacks.onClipboard(ctx->callbacks.userdata,
                                   resp->requestedFormatData,
                                   resp->common.dataLen,
                                   CF_UNICODETEXT);
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

    if (ctx->clipData && ctx->clipLen > 0) {
        resp.common.msgFlags     = CB_RESPONSE_OK;
        resp.common.dataLen      = (UINT32)ctx->clipLen;
        resp.requestedFormatData = (BYTE *)ctx->clipData;
        rdp_verbose("clipboard: serving %zu bytes for format 0x%08x",
                    ctx->clipLen, req->requestedFormatId);
    } else {
        resp.common.msgFlags = CB_RESPONSE_FAIL;
        rdp_verbose("clipboard: data request but nothing held");
    }
    cliprdr->ServerFormatDataResponse(cliprdr, &resp);
    return CHANNEL_RC_OK;
}

/* ── Audio activated callback ──────────────────────────────────────────── */

static void rdpsnd_activated(RdpsndServerContext *rdpsnd) {
    RDPPeerContext *ctx = (RDPPeerContext *)rdpsnd->data;
    /* Pick the first mutually-supported PCM format. */
    for (size_t i = 0; i < rdpsnd->num_client_formats; i++) {
        for (size_t j = 0; j < rdpsnd->num_server_formats; j++) {
            if (audio_format_compatible(&rdpsnd->server_formats[j],
                                        &rdpsnd->client_formats[i])) {
                rdpsnd->SelectFormat(rdpsnd, (UINT16)i);
                ctx->audioReady = true;
                rdp_verbose("audio format negotiated (client idx %zu)", i);
                return;
            }
        }
    }
    rdp_verbose("no compatible audio format found");
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
        ctx->cliprdr->ClientFormatList         = cliprdr_client_format_list;
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

    /* Audio. Advertise ONLY raw PCM matching what AudioCapture produces
     * (48 kHz, stereo, signed 16-bit). server_rdpsnd_get_formats would also
     * offer compressed codecs (AAC/ADPCM/GSM); if one were negotiated, our
     * raw-PCM SendSamples would feed the client undecodable garbage.
     * Must be heap-allocated: rdpsnd_server_context_free() calls free() on
     * server_formats, so a static array would crash on teardown. */
    ctx->rdpsnd = rdpsnd_server_context_new(ctx->vcm);
    if (ctx->rdpsnd) {
        AUDIO_FORMAT *pcm = (AUDIO_FORMAT *)calloc(1, sizeof(AUDIO_FORMAT));
        if (pcm) {
            pcm->wFormatTag      = WAVE_FORMAT_PCM;
            pcm->nChannels       = 2;
            pcm->nSamplesPerSec  = 48000;
            pcm->nAvgBytesPerSec = 48000 * 2 * 2;
            pcm->nBlockAlign     = 2 * 2;
            pcm->wBitsPerSample  = 16;
        }
        ctx->rdpsnd->data               = ctx;
        ctx->rdpsnd->Activated          = rdpsnd_activated;
        ctx->rdpsnd->server_formats     = pcm;
        ctx->rdpsnd->num_server_formats = pcm ? 1 : 0;
        ctx->rdpsnd->src_format         = pcm;
        if (ctx->rdpsnd->Initialize(ctx->rdpsnd, TRUE) != CHANNEL_RC_OK) {
            rdp_verbose("audio channel init failed");
            rdpsnd_server_context_free(ctx->rdpsnd);
            ctx->rdpsnd = NULL;
        } else { rdp_verbose("audio channel opened"); }
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
    if (ctx->callbacks.onReady)
        ctx->callbacks.onReady(ctx->callbacks.userdata, w, h, d);
    return TRUE;
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

    /* Advertise the available format; the client requests the bytes on paste. */
    CLIPRDR_FORMAT fmt   = { .formatId = (UINT32)format, .formatName = NULL };
    CLIPRDR_FORMAT_LIST list = {0};
    list.common.msgFlags = CB_RESPONSE_OK;
    list.numFormats      = 1;
    list.formats         = &fmt;
    /* Called from the clipboard poll thread; serialize against the transport. */
    pthread_mutex_lock(&ctx->xportLock);
    ctx->cliprdr->ServerFormatList(ctx->cliprdr, &list);
    pthread_mutex_unlock(&ctx->xportLock);
    rdp_verbose("clipboard: advertised format 0x%08x (%zu bytes held)", format, len);
    return true;
}
