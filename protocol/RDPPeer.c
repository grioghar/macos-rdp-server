#define RDP_LOG_COMPONENT "peer"
#include "logging/RDPLog.h"
#include "protocol/RDPPeer.h"
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/codec/h264.h>
#include <winpr/ssl.h>
#include <string.h>
#include <stdlib.h>

static void peer_apply_settings(freerdp_peer *peer) {
    rdpSettings *s = peer->context->settings;
    freerdp_settings_set_bool(s, FreeRDP_NetworkAutoDetect,       FALSE);
    freerdp_settings_set_bool(s, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GfxH264,                 TRUE);
    freerdp_settings_set_bool(s, FreeRDP_GfxAVC444,               FALSE);
    freerdp_settings_set_bool(s, FreeRDP_GfxSmallCache,           FALSE);
    freerdp_settings_set_bool(s, FreeRDP_RemoteConsoleAudio,      TRUE);
    freerdp_settings_set_bool(s, FreeRDP_SoundSystem,             TRUE);
    freerdp_settings_set_bool(s, FreeRDP_AudioCapture,            FALSE);
    freerdp_settings_set_bool(s, FreeRDP_UseRdpSecurityLayer,     FALSE);
    freerdp_settings_set_bool(s, FreeRDP_TLSSecLevel,             1);
    freerdp_settings_set_uint32(s, FreeRDP_ColorDepth,            32);
    rdp_debug("peer settings applied");
}

static BOOL context_new(freerdp_peer *peer, rdpContext *ctx) {
    (void)peer; (void)ctx;
    rdp_debug("peer context allocated");
    return TRUE;
}

static void context_free(freerdp_peer *peer, rdpContext *ctx) {
    RDPPeerContext *c = (RDPPeerContext *)ctx;
    if (c->gfx)    { rdpgfx_server_context_free(c->gfx);    c->gfx    = NULL; }
    if (c->cliprdr){ cliprdr_server_context_free(c->cliprdr);c->cliprdr= NULL; }
    if (c->rdpsnd) { rdpsnd_server_context_free(c->rdpsnd);  c->rdpsnd = NULL; }
    rdp_debug("peer context freed");
    (void)peer;
}

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

static UINT gfx_caps_advertise(RdpgfxServerContext *gfx,
                                const RDPGFX_CAPS_ADVERTISE_PDU *pdu) {
    RDPPeerContext *ctx = (RDPPeerContext *)gfx->custom;
    RDPGFX_CAPS_CONFIRM_PDU confirm = {0};

    rdp_verbose("GFX caps advertise: %u cap sets", pdu->capsSetCount);

    for (UINT16 i = 0; i < pdu->capsSetCount; i++) {
        rdp_debug("  caps[%u]: version=0x%08x flags=0x%08x",
                  i, pdu->capsSets[i].version, pdu->capsSets[i].flags);
        if (pdu->capsSets[i].version == RDPGFX_CAPVERSION_8) {
            confirm.capsSet.version = RDPGFX_CAPVERSION_8;
            confirm.capsSet.flags   = pdu->capsSets[i].flags;
            break;
        }
    }
    if (!confirm.capsSet.version) {
        confirm.capsSet.version = RDPGFX_CAPVERSION_7;
        rdp_verbose("GFX: falling back to capsV7");
    } else {
        rdp_verbose("GFX: confirmed capsV8 flags=0x%08x", confirm.capsSet.flags);
    }

    UINT rc = gfx->CapsConfirm(gfx, &confirm);
    if (rc != CHANNEL_RC_OK) {
        rdp_error("GFX CapsConfirm failed: %u", rc);
        return rc;
    }

    rdpSettings *s = ctx->base.peer->context->settings;
    uint32_t w = freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth);
    uint32_t h = freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);
    rdp_verbose("GFX: creating surface %ux%u id=%u", w, h, ctx->surfaceId);

    RDPGFX_CREATE_SURFACE_PDU createSurface = {0};
    createSurface.surfaceId   = ctx->surfaceId;
    createSurface.width       = (UINT16)w;
    createSurface.height      = (UINT16)h;
    createSurface.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
    gfx->CreateSurface(gfx, &createSurface);

    RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU mapSurface = {0};
    mapSurface.surfaceId     = ctx->surfaceId;
    mapSurface.outputOriginX = 0;
    mapSurface.outputOriginY = 0;
    gfx->MapSurfaceToOutput(gfx, &mapSurface);

    ctx->gfxReady = true;
    rdp_info("GFX pipeline ready (%ux%u)", w, h);
    return CHANNEL_RC_OK;
}

static UINT cliprdr_client_format_list(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_FORMAT_LIST *list) {
    rdp_verbose("clipboard: client advertised %u formats", list->numFormats);
    for (UINT32 i = 0; i < list->numFormats; i++)
        rdp_debug("  format[%u]: id=%u name=%s", i,
                  list->formats[i].formatId,
                  list->formats[i].formatName ?: "(unnamed)");
    CLIPRDR_FORMAT_LIST_RESPONSE resp = { .msgFlags = CB_RESPONSE_OK };
    cliprdr->ServerFormatListResponse(cliprdr, &resp);
    return CHANNEL_RC_OK;
}

static UINT cliprdr_client_format_data(CliprdrServerContext *cliprdr,
                                        const CLIPRDR_FORMAT_DATA_RESPONSE *resp) {
    RDPPeerContext *ctx = (RDPPeerContext *)cliprdr->custom;
    if (resp->msgFlags & CB_RESPONSE_OK && ctx->callbacks.onClipboard) {
        rdp_verbose("clipboard: received %u bytes from client", resp->dataLen);
        ctx->callbacks.onClipboard(ctx->callbacks.userdata,
                                   resp->requestedFormatData,
                                   resp->dataLen, CF_UNICODETEXT);
    }
    return CHANNEL_RC_OK;
}

static BOOL peer_post_connect(freerdp_peer *peer) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    rdp_verbose("post-connect: opening virtual channels");

    ctx->gfx = rdpgfx_server_context_new(peer->context->vcm);
    if (!ctx->gfx) { rdp_error("rdpgfx_server_context_new failed"); return FALSE; }
    ctx->gfx->custom        = ctx;
    ctx->gfx->CapsAdvertise = gfx_caps_advertise;
    ctx->surfaceId          = 1;
    if (ctx->gfx->Open(ctx->gfx) != CHANNEL_RC_OK) {
        rdp_verbose("GFX channel open failed — client may not support it");
        rdpgfx_server_context_free(ctx->gfx);
        ctx->gfx = NULL;
    } else {
        rdp_verbose("GFX channel opened");
    }

    ctx->cliprdr = cliprdr_server_context_new(peer->context->vcm);
    if (ctx->cliprdr) {
        ctx->cliprdr->custom                   = ctx;
        ctx->cliprdr->ClientFormatList         = cliprdr_client_format_list;
        ctx->cliprdr->ClientFormatDataResponse = cliprdr_client_format_data;
        if (ctx->cliprdr->Open(ctx->cliprdr) != CHANNEL_RC_OK) {
            rdp_verbose("clipboard channel open failed");
            cliprdr_server_context_free(ctx->cliprdr);
            ctx->cliprdr = NULL;
        } else {
            rdp_verbose("clipboard channel opened");
        }
    }

    ctx->rdpsnd = rdpsnd_server_context_new(peer->context->vcm);
    if (ctx->rdpsnd) {
        if (ctx->rdpsnd->Initialize(ctx->rdpsnd, TRUE) != CHANNEL_RC_OK) {
            rdp_verbose("audio channel init failed");
            rdpsnd_server_context_free(ctx->rdpsnd);
            ctx->rdpsnd = NULL;
        } else {
            rdp_verbose("audio channel opened");
        }
    }

    return TRUE;
}

static BOOL peer_activate(freerdp_peer *peer) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    rdpSettings    *s   = peer->context->settings;
    ctx->activated = true;

    uint32_t w = freerdp_settings_get_uint32(s, FreeRDP_DesktopWidth);
    uint32_t h = freerdp_settings_get_uint32(s, FreeRDP_DesktopHeight);
    uint32_t d = freerdp_settings_get_uint32(s, FreeRDP_ColorDepth);
    rdp_info("peer activated: %ux%u @%ubpp", w, h, d);

    if (ctx->callbacks.onReady)
        ctx->callbacks.onReady(ctx->callbacks.userdata, w, h, d);
    return TRUE;
}

freerdp_peer *rdp_peer_create(int fd, const RDPPeerCallbacks *callbacks) {
    winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT);
    rdp_verbose("creating peer for fd=%d", fd);

    freerdp_peer *peer = freerdp_peer_new(fd);
    if (!peer) { rdp_error("freerdp_peer_new failed"); return NULL; }

    peer->context_size = sizeof(RDPPeerContext);
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
    peer->input->KeyboardEvent      = peer_keyboard;
    peer->input->MouseEvent         = peer_mouse;
    peer->input->ExtendedMouseEvent = peer_mouse_ex;

    if (!peer->Initialize(peer)) {
        rdp_error("peer Initialize failed (TLS/NLA handshake error)");
        freerdp_peer_context_free(peer);
        freerdp_peer_free(peer);
        return NULL;
    }

    rdp_verbose("peer initialized successfully");
    return peer;
}

void rdp_peer_destroy(freerdp_peer *peer) {
    if (!peer) return;
    rdp_verbose("destroying peer");
    freerdp_peer_context_free(peer);
    freerdp_peer_free(peer);
}

bool rdp_peer_run_once(freerdp_peer *peer) {
    if (!peer->CheckFileDescriptor(peer)) {
        rdp_verbose("CheckFileDescriptor returned false — connection closed");
        return false;
    }
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (ctx->gfx)    ctx->gfx->CheckFileDescriptor(ctx->gfx);
    if (ctx->cliprdr) ctx->cliprdr->CheckFileDescriptor(ctx->cliprdr);
    return true;
}

bool rdp_peer_send_h264_frame(freerdp_peer *peer,
                               const uint8_t *data, size_t len,
                               uint32_t width, uint32_t height) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->gfxReady || !ctx->gfx) {
        rdp_debug("h264 send skipped: GFX not ready");
        return false;
    }

    RECTANGLE_16 rect = {0, 0, (UINT16)width, (UINT16)height};
    RDPGFX_AVC420_BITMAP_STREAM avc = {0};
    avc.data   = (BYTE *)data;
    avc.length = (UINT32)len;
    avc.meta.numRegionRects   = 1;
    avc.meta.regionRects      = &rect;
    avc.meta.quantQualityVals = NULL;

    RDPGFX_SURFACE_COMMAND cmd = {0};
    cmd.surfaceId = ctx->surfaceId;
    cmd.codecId   = RDPGFX_CODECID_AVC420;
    cmd.format    = GFX_PIXEL_FORMAT_XRGB_8888;
    cmd.left      = 0; cmd.top = 0;
    cmd.right     = (UINT16)width;
    cmd.bottom    = (UINT16)height;
    cmd.length    = (UINT32)len;
    cmd.data      = (BYTE *)data;
    cmd.extra     = &avc;

    UINT rc = ctx->gfx->SurfaceCommand(ctx->gfx, &cmd);
    if (rc != CHANNEL_RC_OK) {
        rdp_error("SurfaceCommand failed: %u", rc);
        return false;
    }
    return true;
}

bool rdp_peer_send_bitmap(freerdp_peer *peer,
                           const uint8_t *bgra, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height) {
    rdp_verbose("sending bitmap update %ux%u at (%u,%u)", width, height, x, y);
    rdpUpdate *update = peer->context->update;
    SURFACE_BITS_COMMAND cmd = {0};
    cmd.destLeft   = (UINT16)x;    cmd.destTop    = (UINT16)y;
    cmd.destRight  = (UINT16)(x + width);
    cmd.destBottom = (UINT16)(y + height);
    cmd.bmp.bpp    = 32;
    cmd.bmp.width  = (UINT16)width;
    cmd.bmp.height = (UINT16)height;
    cmd.bmp.bitmapData       = (BYTE *)bgra;
    cmd.bmp.bitmapDataLength = width * height * 4;
    cmd.bmp.codecID          = RDP_CODEC_ID_NONE;
    return update->SurfaceBits(update->context, &cmd);
}

bool rdp_peer_send_audio(freerdp_peer *peer,
                          const int16_t *samples, uint32_t frame_count) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->rdpsnd) { rdp_debug("audio send skipped: no rdpsnd channel"); return false; }
    return audio_redirect_send(ctx->rdpsnd, samples, frame_count, 48000, 2);
}

bool rdp_peer_send_clipboard(freerdp_peer *peer,
                              const uint8_t *data, size_t len,
                              uint32_t format) {
    RDPPeerContext *ctx = (RDPPeerContext *)peer->context;
    if (!ctx->cliprdr) { rdp_verbose("clipboard send skipped: no channel"); return false; }
    rdp_verbose("sending clipboard to client: format=0x%08x len=%zu", format, len);

    CLIPRDR_FORMAT formats[] = {{.formatId = (UINT32)format, .formatName = NULL}};
    CLIPRDR_FORMAT_LIST list  = { .msgFlags = CB_RESPONSE_OK, .numFormats = 1, .formats = formats };
    ctx->cliprdr->ServerFormatList(ctx->cliprdr, &list);

    CLIPRDR_FORMAT_DATA_RESPONSE resp = {
        .msgFlags            = CB_RESPONSE_OK,
        .dataLen             = (UINT32)len,
        .requestedFormatData = (BYTE *)data,
    };
    ctx->cliprdr->ServerFormatDataResponse(ctx->cliprdr, &resp);
    return true;
}
