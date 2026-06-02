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
#include <winpr/wtsapi.h>

typedef struct rdp_peer_context RDPPeerContext;

typedef void (*RDPPeerInputCallback)(void *userdata, uint16_t flags, uint16_t code);
typedef void (*RDPPeerMouseCallback)(void *userdata, uint16_t flags, uint16_t x, uint16_t y);
typedef void (*RDPPeerClipboardCallback)(void *userdata, const uint8_t *data, size_t len, uint32_t format);
typedef void (*RDPPeerReadyCallback)(void *userdata, uint32_t width, uint32_t height, uint32_t colorDepth);
typedef void (*RDPPeerKeyframeCallback)(void *userdata);

typedef struct {
    RDPPeerInputCallback     onKeyboard;
    RDPPeerMouseCallback     onMouse;
    RDPPeerMouseCallback     onMouseEx;
    RDPPeerClipboardCallback onClipboard;
    RDPPeerReadyCallback     onReady;
    /* Asks the encoder to emit an IDR keyframe ASAP. Invoked when the GFX channel
     * becomes ready (frames encoded earlier were discarded, so the first sent
     * frame must be a keyframe) or when a delta arrives before any keyframe. */
    RDPPeerKeyframeCallback  onKeyframeRequest;
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
    uint32_t             frameId;    /* monotonic GFX frame id for StartFrame/EndFrame */
    /* Serializes ALL writes to the single RDP transport (TLS socket). FreeRDP is
     * NOT thread-safe for concurrent sends: the encoder thread (GFX SurfaceCommand),
     * the run-loop thread (CheckFileDescriptor + VCM pump + GFX handle_messages),
     * the clipboard poll thread (cliprdr ServerFormatList) and the audio thread
     * (rdpsnd SendSamples) all write the same socket. Unsynchronized, their bytes
     * interleave and the GFX bytestream desyncs -> mstsc decodes a "packet of type
     * Unknown" and drops the session (Reason 3334). Every transport write MUST hold
     * this lock. RECURSIVE so a run-loop callback (e.g. cliprdr) can re-enter while
     * the loop already holds it. */
    pthread_mutex_t      xportLock;
    bool                 gfxOpened;  /* GFX DVC Open() succeeded (drdynvc ready) */
    bool                 gfxReady;   /* client sent GFX caps; surface mapped */
    bool                 sentKeyframe;     /* a keyframe has been sent this session */
    bool                 keyframeRequested;/* debounce: requested an IDR, awaiting it */
    bool                 outputSuppressed; /* client minimized: Suppress Output PDU -> pause sends */
    bool                 activated;
    bool                 audioReady; /* set by rdpsnd Activated callback */
    /* Clipboard: the host's current data, advertised via Format List and held
     * until the client sends a Format Data Request (MS-RDPECLIP flow). Owned
     * copy — the source pasteboard pointer is only valid during the send call. */
    uint8_t             *clipData;
    size_t               clipLen;
    uint32_t             clipFormat;
    /* The format id we asked the client for via ServerFormatDataRequest after it
     * advertised a Format List (Win->Mac paste). The matching ClientFormatDataResponse
     * carries no format id of its own, so we remember what we requested. */
    uint32_t             clipReqFormat;
    /* RDPDR (MS-RDPEFS) drive-redirection static VC. Non-NULL only when
     * RDP_RDPDR_ENABLED=1. Raw WTS channel; protocol state machine runs in C. */
    HANDLE               rdpdrChannel;   /* WTSVirtualChannelOpen handle */
    HANDLE               rdpdrEvent;     /* WTSVirtualEventHandle for the run loop */
    int                  rdpdrState;     /* RdpdrHandshakeState enum (int for C compat) */
    uint16_t             rdpdrClientId;  /* client-assigned id from ANNOUNCE_REPLY */
    uint32_t             rdpdrNextReqId; /* monotonic IRP request id counter */
    /* Pending IRP request table — up to 8 in-flight requests at once (enumerate
     * only, so one per drive is typical). Cleared when the completion arrives. */
#define RDPDR_MAX_PENDING 8
    struct {
        uint32_t requestId;  /* 0 = slot free */
        uint32_t deviceId;
        char     path[64];   /* requested path, for logging */
    } rdpdrPending[RDPDR_MAX_PENDING];
    /* cliprdr handshake complete (client sent its Capabilities/Format List). The
     * Mac->Win advertise (ServerFormatList, called from the clipboard POLL thread)
     * MUST NOT run before this — sending a Format List before the channel's send
     * state is initialized corrupts FreeRDP's cliprdr stream and aborts the daemon
     * (WinPR Stream_Write_UINT32 assert). Set true in the client caps/format-list
     * callbacks; gates rdp_peer_send_clipboard. */
    bool                 clipReady;
};

freerdp_peer *rdp_peer_create(int fd, const RDPPeerCallbacks *callbacks);
void          rdp_peer_destroy(freerdp_peer *peer);
bool          rdp_peer_run_once(freerdp_peer *peer);

/* Read the logon credentials the client supplied in the RDP info packet (valid
 * once the peer has activated — i.e. inside/after the onReady callback). The
 * out-pointers receive pointers OWNED by the FreeRDP peer settings (do not free;
 * valid for the peer's lifetime). Any out value may be NULL if the client sent
 * none. Pass NULL for fields you don't need. NEVER log the returned password. */
void rdp_peer_get_credentials(freerdp_peer *peer,
                              const char **username,
                              const char **password,
                              const char **domain);

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

/* Negotiated client audio playback sample rate in Hz, or 0 if audio is not yet
 * activated (rdpsnd Activated callback has not fired, or no compatible format).
 *
 * rdpsnd does NOT resample: the client plays the PCM bytes we send at the rate
 * of the format IT selected. AudioCapture polls this to drive its resampler so
 * the captured/sent rate equals the client's playback rate (otherwise the audio
 * is pitch-shifted). Safe to call from any thread. */
uint32_t rdp_peer_get_audio_rate(freerdp_peer *peer);

bool rdp_peer_send_clipboard(freerdp_peer *peer,
                              const uint8_t *data, size_t len,
                              uint32_t format);

/* Tell the client to render the default system pointer CLIENT-SIDE, so the cursor
 * tracks the local mouse smoothly instead of being tied to the (30fps) video. */
void rdp_peer_send_default_cursor(freerdp_peer *peer);

/*
 * RDPDR (drive redirection) channel support.
 *
 * rdp_peer_open_rdpdr  — open the "rdpdr" WTS static virtual channel and send
 *   SERVER_ANNOUNCE. Should be called from peer_post_connect (or later once the
 *   VCM is ready). Returns true on success.
 *
 * rdp_peer_pump_rdpdr  — drain inbound rdpdr data and advance the MS-RDPEFS
 *   state machine. Call from the peer run loop whenever the rdpdr channel event
 *   is signaled, under xportLock.
 *
 * Both are no-ops when RDP_RDPDR_ENABLED != "1".
 */
bool rdp_peer_open_rdpdr(freerdp_peer *peer);
void rdp_peer_pump_rdpdr(freerdp_peer *peer);

/* Create Desktop placeholder folders for each redirected client drive.
 * Implemented in protocol/RDPDriveMount.m (ObjC/Foundation). */
void rdp_drive_mount_placeholder(const char *driveName);

/* Send the ACTUAL current cursor shape as an RDP color-pointer PDU so mstsc
 * renders the correct shape (I-beam, resize, hand, …) client-side, lag-free.
 *
 * `bgra` is a 32-bit BGRA premultiplied, TOP-LEFT-origin, tightly packed
 * (stride == w*4) bitmap; w/h are in pixels (capped ~96; >96 uses the Large
 * pointer PDU). hotX/hotY are the hotspot in bitmap pixels. Internally builds a
 * bottom-up xor mask + a 1bpp AND mask from the alpha channel.
 *
 * Holds xportLock (transport write). No-op if the peer is not yet activated or
 * output is suppressed. Safe to call repeatedly; the caller should de-dupe
 * (only call when the shape actually changes). */
void rdp_peer_send_cursor_shape(freerdp_peer *peer,
                                const uint8_t *bgra, uint32_t w, uint32_t h,
                                uint16_t hotX, uint16_t hotY);
