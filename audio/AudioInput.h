#pragma once

/*
 * AudioInput — MS-RDPEAI (AUDIO_INPUT virtual channel) public interface.
 *
 * Provides C-callable wrappers so RDPPeer.c (pure C) can open, pump, and
 * close the audio-input channel without importing ObjC headers.
 *
 * All functions are no-ops if RDP_AUDIO_INPUT != "1".
 */

#include <stdbool.h>
#include <freerdp/freerdp.h>
#include <winpr/wtypes.h>   /* HANDLE */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open the AUDIO_INPUT static virtual channel for the given peer, send the
 * MS-RDPEAI VERSION + FORMATS handshake, and start a CoreAudio AudioQueue for
 * Mac speaker playback.
 *
 * Returns an opaque handle on success, NULL on failure or when gated off.
 * The handle is an ARC-retained ObjC object stored as void*.
 */
void *rdp_audio_input_open(freerdp_peer *peer);

/*
 * Return the WTS channel event handle (for WaitForMultipleObjects in the run
 * loop). Returns NULL if the channel is not open.
 */
HANDLE rdp_audio_input_event(void *handle);

/*
 * Drain pending DATA PDUs from the channel and feed PCM to CoreAudio.
 * Call from the peer run loop whenever the channel event fires.
 * No-op if handle is NULL.
 */
void rdp_audio_input_pump(void *handle);

/*
 * Stop CoreAudio playback, release the channel, and free the handle.
 * Safe to call with a NULL handle or multiple times.
 * After this call the handle is invalid and must not be used.
 */
void rdp_audio_input_close(void *handle);

#ifdef __cplusplus
}
#endif

/* ── ObjC class interface (only when compiling ObjC/ObjC++) ────────────── */
#ifdef __OBJC__
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * RDPAudioInput — the ObjC implementation backing the C wrappers above.
 *
 * Direct ObjC callers (e.g. tests) may use this interface; production callers
 * should prefer the C wrappers for C-file compatibility.
 */
@interface RDPAudioInput : NSObject

+ (nullable instancetype)openForPeer:(freerdp_peer *)peer;
- (HANDLE)eventHandle;
- (void)pump;
- (void)close;

@end

NS_ASSUME_NONNULL_END
#endif /* __OBJC__ */
