#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <freerdp/server/rdpsnd.h>

/*
 * Send interleaved signed-16 stereo PCM at 48 kHz to the client.
 * Returns false if the audio channel is not yet ready (format not negotiated).
 * Caller must check rdp_peer_send_audio() which guards on audioReady.
 */
bool audio_redirect_send(RdpsndServerContext *ctx,
                          const int16_t *samples,
                          size_t frame_count);
