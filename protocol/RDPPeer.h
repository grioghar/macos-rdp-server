#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/channels/wtsvc.h>

typedef struct rdp_peer_context RDPPeerContext;

typedef void (*RDPPeerInputCallback)(void *userdata, uint16_t flags, uint16_t code);
typedef void (*RDPPeerMouseCallback)(void *userdata, uint16_t flags, uint16_t x, uint16_t y);
typedef void (*RDPPeerClipboardCallback)(void *userdata, const uint8_t *data, size_t len, uint32_t format);
typedef void (*RDPPeerReadyCallback)(void *userdata, uint32_t width, uint32_t height, uint32_t colorDepth);

typedef struct {
    RDPPeerInputCallback     onKeyboard;
    RDPPeerMouseCallback     onMouse;
    RDPPeerMouseCallback     onMouseEx;
    RDPPeerClipboardCallback onClipboard;
    RDPPeerReadyCallback     onReady;
    void *userdata;
} RDPPeerCallbacks;

struct rdp_peer_context {
    rdpContext          base;       /* MUST be first */
    RDPPeerCallbacks    callbacks;
    HANDLE              vcm;        /* WTSOpenServerA handle — owns gfx/cliprdr/rdpsnd */
    RdpgfxServerContext *gfx;
    CliprdrServerContext *cliprdr;
    RdpsndServerContext  *rdpsnd;
    uint32_t             surfaceId;
    /* Serializes ALL access to the GFX context (SurfaceCommand from the encoder
     * thread vs handle_messages from the run-loop thread). The GFX channel is
     * also put in external-thread mode so its internal thread never runs. */
    pthread_mutex_t      gfxLock;
    bool                 gfxOpened;  /* GFX DVC Open() succeeded (drdynvc ready) */
    bool                 gfxReady;   /* client sent GFX caps; surface mapped */
    bool                 activated;
    bool                 audioReady; /* set by rdpsnd Activated callback */
    /* Clipboard: the host's current data, advertised via Format List and held
     * until the client sends a Format Data Request (MS-RDPECLIP flow). Owned
     * copy — the source pasteboard pointer is only valid during the send call. */
    uint8_t             *clipData;
    size_t               clipLen;
    uint32_t             clipFormat;
};

freerdp_peer *rdp_peer_create(int fd, const RDPPeerCallbacks *callbacks);
void          rdp_peer_destroy(freerdp_peer *peer);
bool          rdp_peer_run_once(freerdp_peer *peer);

/*
 * Send an AVC420 (H.264) frame.
 *
 * The H.264 bitstream always encodes the full surface (VideoToolbox encodes
 * the whole IOSurface), so the surface-command destination is always the full
 * surface. The damage region is signalled via the AVC420 metablock regionRects:
 *   - keyframe  -> region = full surface (entire picture is fresh)
 *   - interframe-> region = dirty rect   (only changed pixels need compositing)
 *
 * A valid quantQualityVals array (one entry per region) is ALWAYS supplied —
 * the FreeRDP server serializer dereferences it unconditionally, so a NULL
 * here is a guaranteed crash on the first frame.
 *
 * dirtyX/Y/W/H are in surface pixels; ignored when isKeyFrame is true.
 */
bool rdp_peer_send_h264_frame(freerdp_peer *peer,
                               const uint8_t *data, size_t len,
                               uint32_t width, uint32_t height,
                               bool isKeyFrame,
                               uint16_t dirtyX, uint16_t dirtyY,
                               uint16_t dirtyW, uint16_t dirtyH);

bool rdp_peer_send_bitmap(freerdp_peer *peer,
                           const uint8_t *bgra, uint32_t x, uint32_t y,
                           uint32_t width, uint32_t height);

bool rdp_peer_send_audio(freerdp_peer *peer,
                          const int16_t *samples, uint32_t frame_count);

bool rdp_peer_send_clipboard(freerdp_peer *peer,
                              const uint8_t *data, size_t len,
                              uint32_t format);
