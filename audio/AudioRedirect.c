#define RDP_LOG_COMPONENT "audio"
#include "logging/RDPLog.h"
#include "audio/AudioRedirect.h"

bool audio_redirect_send(RdpsndServerContext *ctx,
                          const int16_t *samples,
                          size_t frame_count) {
    if (!ctx || !ctx->SendSamples) return false;
    /* SendSamples(context, buf, nframes, wTimestamp) — FreeRDP 3.x signature.
       Timestamp 0 is valid; the channel assigns block numbers internally. */
    UINT rc = ctx->SendSamples(ctx, samples, frame_count, 0);
    if (rc != CHANNEL_RC_OK) {
        rdp_debug("SendSamples returned %u", rc);
        return false;
    }
    return true;
}
