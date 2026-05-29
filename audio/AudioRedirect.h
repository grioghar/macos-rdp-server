#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <freerdp/server/rdpsnd.h>

/* Encode and queue PCM audio for the RDP RDPSND channel. */
bool audio_redirect_send(RdpsndServerContext *ctx,
                          const int16_t *samples,
                          uint32_t frame_count,
                          uint32_t sample_rate,
                          uint16_t channels);
