/*
 * AudioInput.m — MS-RDPEAI client→server audio input redirection.
 *
 * mstsc sends the user's microphone audio over the AUDIO_INPUT static virtual
 * channel when "Record from this computer" is enabled in mstsc options.
 * We receive DATA PDUs containing raw 16kHz mono 16-bit PCM and play them on
 * the Mac via CoreAudio AudioQueue.
 *
 * Feature gate: RDP_AUDIO_INPUT=1 (default off).
 */

#define RDP_LOG_COMPONENT "audio_input"
#include "logging/RDPLog.h"
#import "audio/AudioInput.h"

#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <winpr/wtsapi.h>
#include <string.h>

/* ── MS-RDPEAI protocol constants ──────────────────────────────────────── */
#define AI_SNDC_DATA         0x0002  /* DATA PDU (client→server, mic audio)  */
#define AI_SNDC_FORMATS      0x0007  /* FORMATS PDU                          */
#define AI_SNDC_VERSION      0x0008  /* VERSION PDU                          */
#define AI_SNDC_OPEN         0x000A  /* OPEN PDU (start capture)             */
#define AI_SNDC_CLOSE        0x0009  /* CLOSE PDU (stop capture)             */
#define AI_VERSION           0x0006  /* MS-RDPEAI protocol version           */
#define AI_WAVE_FORMAT_PCM   0x0001  /* WAVE_FORMAT_PCM (uncompressed PCM)   */
#define AI_CHANNEL_NAME      "AUDIO_INPUT"

/* ── Implementation limits ─────────────────────────────────────────────── */
/* Non-blocking read buffer — must be large enough for any single DATA PDU.
 * mstsc typically sends ~640 bytes per 20ms, but can burst; 64 KB is safe. */
#define AI_READ_BUF_BYTES    (64 * 1024)
/* Per-PDU AudioQueue buffer upper bound. DATA PDUs are typically 64–4096 bytes. */
#define AI_MAX_PDU_PCM_BYTES (16 * 1024)

/* ── RDPAudioInput ObjC class ──────────────────────────────────────────── */

@interface RDPAudioInput () {
    HANDLE          _channel;       /* WTS static VC handle            */
    HANDLE          _chanEvent;     /* signaled when data is available */
    AudioQueueRef   _queue;         /* CoreAudio output queue          */
    BOOL            _queueRunning;
}
@end

@implementation RDPAudioInput

/* ── Open + negotiate ─────────────────────────────────────────────────── */

+ (nullable instancetype)openWithVCM:(HANDLE)vcm {
    if (!vcm || vcm == INVALID_HANDLE_VALUE) return nil;

    /* Open the AUDIO_INPUT static virtual channel using the already-open VCM
     * from the peer context (ctx->vcm in RDPPeer.c). */
    HANDLE ch = WTSVirtualChannelOpen(vcm, WTS_CURRENT_SESSION,
                                      (LPSTR)AI_CHANNEL_NAME);
    if (!ch || ch == INVALID_HANDLE_VALUE) {
        rdp_verbose("audio_input: channel '%s' not available "
                    "(client may not have enabled mic redirection)", AI_CHANNEL_NAME);
        return nil;
    }

    RDPAudioInput *ai = [[RDPAudioInput alloc] init];
    if (!ai) { WTSVirtualChannelClose(ch); return nil; }
    ai->_channel = ch;

    /* Retrieve the event handle for the run-loop. */
    void  *evp = NULL;
    ULONG  evl = sizeof(evp);
    if (WTSVirtualChannelQuery(ch, WTSVirtualEventHandle, &evp, &evl) && evp) {
        ai->_chanEvent = (HANDLE)*(void **)evp;
        WTSFreeMemory(evp);
    } else {
        rdp_verbose("audio_input: could not query channel event handle");
    }

    /* ── VERSION PDU (server→client): Header(4) + Version(2) = 6 bytes ── */
    {
        uint8_t pdu[6];
        pdu[0] = AI_SNDC_VERSION & 0xFF;
        pdu[1] = (AI_SNDC_VERSION >> 8) & 0xFF;
        pdu[2] = 0; pdu[3] = 0;   /* Reserved */
        pdu[4] = AI_VERSION & 0xFF;
        pdu[5] = (AI_VERSION >> 8) & 0xFF;
        ULONG written = 0;
        if (!WTSVirtualChannelWrite(ch, (PCHAR)pdu, sizeof(pdu), &written)
            || written != sizeof(pdu)) {
            rdp_verbose("audio_input: VERSION PDU write failed");
            [ai close]; return nil;
        }
        rdp_verbose("audio_input: -> VERSION %u", AI_VERSION);
    }

    /* ── FORMATS PDU (server→client) ─────────────────────────────────────
     * Header(4) + NumFormats(4) + one SNDFORMAT(18) = 26 bytes.
     * We advertise a single format: 16 kHz, mono, 16-bit signed PCM.
     * mstsc will select this (or its closest match) and send DATA PDUs in it. */
    {
        uint8_t pdu[26];
        uint8_t *p = pdu;

        /* Header */
        *p++ = AI_SNDC_FORMATS & 0xFF; *p++ = (AI_SNDC_FORMATS >> 8) & 0xFF;
        *p++ = 0; *p++ = 0; /* Reserved */

        /* NumFormats (4 LE) */
        *p++ = 1; *p++ = 0; *p++ = 0; *p++ = 0;

        /* SNDFORMAT — MS-RDPEAI §2.2.1 */
        uint16_t tag     = AI_WAVE_FORMAT_PCM;
        uint16_t ch_n    = 1;
        uint32_t rate    = 16000;
        uint32_t avgBps  = 32000;   /* rate * ch_n * 2 */
        uint16_t align   = 2;       /* ch_n * 2 */
        uint16_t bits    = 16;
        uint16_t cbSize  = 0;

#define W16(v) do { *p++ = (uint8_t)((v)&0xFF); *p++ = (uint8_t)(((v)>>8)&0xFF); } while(0)
#define W32(v) do { W16((uint16_t)((v)&0xFFFF)); W16((uint16_t)(((v)>>16)&0xFFFF)); } while(0)
        W16(tag); W16(ch_n); W32(rate); W32(avgBps); W16(align); W16(bits); W16(cbSize);
#undef W16
#undef W32

        ULONG written = 0;
        ULONG len = (ULONG)(p - pdu);
        if (!WTSVirtualChannelWrite(ch, (PCHAR)pdu, len, &written)
            || written != len) {
            rdp_verbose("audio_input: FORMATS PDU write failed");
            [ai close]; return nil;
        }
        rdp_verbose("audio_input: -> FORMATS (16kHz mono 16-bit PCM)");
    }

    /* ── CoreAudio AudioQueue for 16 kHz mono 16-bit signed PCM playback ── */
    AudioStreamBasicDescription asbd = {
        .mSampleRate       = 16000.0,
        .mFormatID         = kAudioFormatLinearPCM,
        .mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
        .mBitsPerChannel   = 16,
        .mChannelsPerFrame = 1,
        .mFramesPerPacket  = 1,
        .mBytesPerPacket   = 2,
        .mBytesPerFrame    = 2,
    };

    OSStatus st = AudioQueueNewOutput(&asbd, audio_queue_cb,
                                      (__bridge void *)ai,
                                      NULL, NULL, 0, &ai->_queue);
    if (st != noErr) {
        rdp_verbose("audio_input: AudioQueueNewOutput failed (%d)", (int)st);
        [ai close]; return nil;
    }

    st = AudioQueueStart(ai->_queue, NULL);
    if (st != noErr) {
        rdp_verbose("audio_input: AudioQueueStart failed (%d)", (int)st);
        [ai close]; return nil;
    }
    ai->_queueRunning = YES;

    rdp_info("audio_input: channel open, CoreAudio playback started");
    return ai;
}

/* ── CoreAudio callback — buffer finished playing ─────────────────────── */

static void audio_queue_cb(void *userData, AudioQueueRef queue,
                            AudioQueueBufferRef buf) {
    /* The buffer has finished playing. Free it — callers allocate one per PDU. */
    (void)userData;
    AudioQueueFreeBuffer(queue, buf);
}

/* ── eventHandle accessor (for run-loop WaitForMultipleObjects) ────────── */

- (HANDLE)eventHandle {
    return _chanEvent;
}

/* ── pump — drain DATA PDUs ───────────────────────────────────────────── */

- (void)pump {
    if (!_channel || _channel == INVALID_HANDLE_VALUE) return;

    static uint8_t buf[AI_READ_BUF_BYTES];
    ULONG bytesRead = 0;

    while (WTSVirtualChannelRead(_channel, 0,
                                  (PCHAR)buf, (ULONG)sizeof(buf), &bytesRead)
           && bytesRead > 0) {

        if (bytesRead < 4) {
            rdp_verbose("audio_input: short PDU (%lu bytes) — discarding",
                        (unsigned long)bytesRead);
            continue;
        }

        uint16_t msgId = (uint16_t)(buf[0] | ((uint16_t)buf[1] << 8));

        if (msgId == AI_SNDC_DATA) {
            /* DATA PDU: 4-byte header + raw PCM payload. */
            if (bytesRead <= 4) continue;
            const uint8_t *pcm    = buf + 4;
            uint32_t       pcmLen = (uint32_t)(bytesRead - 4);
            [self playPCM:pcm length:pcmLen];
        } else if (msgId == AI_SNDC_OPEN) {
            rdp_verbose("audio_input: <- OPEN");
        } else if (msgId == AI_SNDC_CLOSE) {
            rdp_verbose("audio_input: <- CLOSE");
        } else if (msgId == AI_SNDC_VERSION) {
            uint16_t ver = (bytesRead >= 6)
                         ? (uint16_t)(buf[4] | ((uint16_t)buf[5] << 8)) : 0;
            rdp_verbose("audio_input: <- VERSION %u", ver);
        } else if (msgId == AI_SNDC_FORMATS) {
            rdp_verbose("audio_input: <- FORMATS (client ack)");
        } else {
            rdp_verbose("audio_input: unknown msgId=0x%04x len=%lu — ignoring",
                        (unsigned)msgId, (unsigned long)bytesRead);
        }
    }
}

/* ── playPCM — enqueue one PCM buffer into CoreAudio ─────────────────── */

- (void)playPCM:(const uint8_t *)data length:(uint32_t)len {
    if (!_queue || !_queueRunning || len == 0) return;
    if (len > AI_MAX_PDU_PCM_BYTES) {
        rdp_verbose("audio_input: oversized PDU (%u bytes) — clamping", len);
        len = AI_MAX_PDU_PCM_BYTES;
    }

    AudioQueueBufferRef buf = NULL;
    OSStatus st = AudioQueueAllocateBuffer(_queue, len, &buf);
    if (st != noErr || !buf) {
        rdp_verbose("audio_input: alloc buffer failed for %u bytes (%d)", len, (int)st);
        return;
    }

    memcpy(buf->mAudioData, data, len);
    buf->mAudioDataByteSize = len;

    /* audio_queue_cb frees the buffer when playback is complete. */
    AudioQueueEnqueueBuffer(_queue, buf, 0, NULL);
}

/* ── close ─────────────────────────────────────────────────────────────── */

- (void)close {
    if (_queue) {
        if (_queueRunning) {
            AudioQueueStop(_queue, TRUE /* immediate */);
            _queueRunning = NO;
        }
        AudioQueueDispose(_queue, TRUE);
        _queue = NULL;
    }
    if (_channel && _channel != INVALID_HANDLE_VALUE) {
        WTSVirtualChannelClose(_channel);
        _channel   = NULL;
        _chanEvent = NULL;
    }
    rdp_verbose("audio_input: closed");
}

- (void)dealloc {
    [self close];
}

@end

/* ── C-callable wrappers (for RDPPeer.c) ──────────────────────────────── */

void *rdp_audio_input_open(HANDLE vcm) {
    RDPAudioInput *ai = [RDPAudioInput openWithVCM:vcm];
    if (!ai) return NULL;
    /* Retain the object for the C caller — released in rdp_audio_input_close. */
    return (__bridge_retained void *)ai;
}

HANDLE rdp_audio_input_event(void *handle) {
    if (!handle) return NULL;
    RDPAudioInput *ai = (__bridge RDPAudioInput *)handle;
    return [ai eventHandle];
}

void rdp_audio_input_pump(void *handle) {
    if (!handle) return;
    RDPAudioInput *ai = (__bridge RDPAudioInput *)handle;
    [ai pump];
}

void rdp_audio_input_close(void *handle) {
    if (!handle) return;
    /* Transfer ownership back to ARC so the object is released after -close. */
    RDPAudioInput *ai = (__bridge_transfer RDPAudioInput *)handle;
    [ai close];
    (void)ai;   /* ARC releases ai here */
}
