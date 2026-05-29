#include "audio/AudioRedirect.h"
#include <freerdp/server/rdpsnd.h>
#include <string.h>
#include <stdlib.h>

bool audio_redirect_send(RdpsndServerContext *ctx,
                          const int16_t *samples,
                          uint32_t frame_count,
                          uint32_t sample_rate,
                          uint16_t channels) {
    if (!ctx) return false;

    /* Negotiate format once on first send. */
    static bool format_set = false;
    if (!format_set) {
        AUDIO_FORMAT fmt = {
            .wFormatTag      = WAVE_FORMAT_PCM,
            .nChannels       = channels,
            .nSamplesPerSec  = sample_rate,
            .nAvgBytesPerSec = sample_rate * channels * sizeof(int16_t),
            .nBlockAlign     = (UINT16)(channels * sizeof(int16_t)),
            .wBitsPerSample  = 16,
            .cbSize          = 0,
        };
        ctx->SelectFormat(ctx, &fmt);
        ctx->SetVolume(ctx, 0xFFFF, 0xFFFF);
        format_set = true;
    }

    size_t byte_count = frame_count * channels * sizeof(int16_t);
    RDPSND_DATA_BLOCK block = {
        .data    = (BYTE *)samples,
        .size    = (UINT32)byte_count,
    };

    return ctx->SendSamples(ctx, &block) == CHANNEL_RC_OK;
}
