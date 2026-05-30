#import "display/FrameEncoder.h"
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <IOSurface/IOSurface.h>
#define RDP_LOG_COMPONENT "encoder"
#include "logging/RDPLog.h"

@interface FrameEncoder ()
@property (nonatomic, assign) VTCompressionSessionRef session;
@property (nonatomic, assign) uint32_t w;
@property (nonatomic, assign) uint32_t h;
@property (nonatomic, assign) uint32_t bitrateKbps;
@property (nonatomic, assign) int64_t  frameIndex;
@property (nonatomic, assign) uint64_t bytesEncoded;
@end

static void vt_callback(void *outputCallbackRefCon, void *sourceFrameRefCon,
                         OSStatus status, VTEncodeInfoFlags infoFlags,
                         CMSampleBufferRef sampleBuffer);

/* Pack/unpack a damage rect into the pointer-sized VT sourceFrameRefCon.
 * void* is 64-bit on every target arch, so four UINT16 fit exactly — this
 * carries the dirty rect to the (async) output callback with zero allocation
 * and no shared-state race. */
static inline void *pack_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uintptr_t p = (uintptr_t)x | ((uintptr_t)y << 16) |
                  ((uintptr_t)w << 32) | ((uintptr_t)h << 48);
    return (void *)p;
}
static inline void unpack_rect(void *refcon, uint16_t *x, uint16_t *y,
                                uint16_t *w, uint16_t *h) {
    uintptr_t p = (uintptr_t)refcon;
    *x = (uint16_t)(p);       *y = (uint16_t)(p >> 16);
    *w = (uint16_t)(p >> 32); *h = (uint16_t)(p >> 48);
}

@implementation FrameEncoder

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                      bitrate:(uint32_t)bitrateKbps {
    if ((self = [super init])) {
        _w = width; _h = height; _bitrateKbps = bitrateKbps;
    }
    return self;
}

- (uint32_t)width  { return _w; }
- (uint32_t)height { return _h; }

- (BOOL)start {
    rdp_verbose("creating VT H.264 session %ux%u @ %u kbps", _w, _h, _bitrateKbps);
    OSStatus err = VTCompressionSessionCreate(kCFAllocatorDefault,
        (int32_t)_w, (int32_t)_h, kCMVideoCodecType_H264,
        NULL, NULL, NULL, vt_callback, (__bridge void *)self, &_session);
    if (err != noErr) {
        rdp_error("VTCompressionSessionCreate failed: %d", (int)err);
        return NO;
    }

    int32_t bitrate = (int32_t)(_bitrateKbps * 1000);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_AverageBitRate,
                         (__bridge CFTypeRef)@(bitrate));
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_RealTime,           kCFBooleanTrue);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_Baseline_AutoLevel);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_H264EntropyMode,
                         kVTH264EntropyMode_CAVLC);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                         (__bridge CFTypeRef)@(120));

    VTCompressionSessionPrepareToEncodeFrames(_session);
    rdp_info("H.264 encoder ready: %ux%u @ %u kbps", _w, _h, _bitrateKbps);
    return YES;
}

- (void)encodeFrame:(IOSurfaceRef)surface
             dirtyX:(uint16_t)dirtyX dirtyY:(uint16_t)dirtyY
             dirtyW:(uint16_t)dirtyW dirtyH:(uint16_t)dirtyH {
    if (!_session || !surface) return;
    CVPixelBufferRef pixbuf = NULL;
    CVReturn cvErr = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault,
                                                      surface, NULL, &pixbuf);
    if (cvErr != kCVReturnSuccess || !pixbuf) {
        rdp_error("CVPixelBufferCreateWithIOSurface failed: %d", cvErr);
        return;
    }
    CMTime pts = CMTimeMake(_frameIndex++, 60);
    void *refcon = pack_rect(dirtyX, dirtyY, dirtyW, dirtyH);
    VTCompressionSessionEncodeFrame(_session, pixbuf, pts, kCMTimeInvalid,
                                    NULL, refcon, NULL);
    CVPixelBufferRelease(pixbuf);
}

- (void)stop {
    if (!_session) return;
    rdp_verbose("stopping encoder after %lld frames (%llu bytes encoded)",
                _frameIndex, _bytesEncoded);
    VTCompressionSessionCompleteFrames(_session, kCMTimeIndefinite);
    VTCompressionSessionInvalidate(_session);
    CFRelease(_session);
    _session = NULL;
}

- (void)handleSampleBuffer:(CMSampleBufferRef)buf
                  dirtyRect:(void *)refcon {
    if (!CMSampleBufferDataIsReady(buf)) return;
    uint16_t dx, dy, dw, dh;
    unpack_rect(refcon, &dx, &dy, &dw, &dh);

    BOOL isKey = NO;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buf, FALSE);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        isKey = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
    }

    NSMutableData *annexB = [NSMutableData data];

    if (isKey) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        size_t count = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, NULL, NULL, &count, NULL);
        rdp_debug("keyframe: %zu parameter sets", count);
        for (size_t i = 0; i < count; i++) {
            const uint8_t *ps; size_t psLen; int naluHeaderLen;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &psLen,
                                                               NULL, &naluHeaderLen);
            uint8_t sc[4] = {0,0,0,1};
            [annexB appendBytes:sc length:4];
            [annexB appendBytes:ps length:psLen];
        }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(buf);
    size_t offset = 0, totalLen = 0;
    char *baseAddr = NULL;
    CMBlockBufferGetDataPointer(block, 0, NULL, &totalLen, &baseAddr);

    while (offset < totalLen) {
        uint32_t naluLen;
        memcpy(&naluLen, baseAddr + offset, 4);
        naluLen = CFSwapInt32BigToHost(naluLen);
        offset += 4;
        uint8_t sc[4] = {0,0,0,1};
        [annexB appendBytes:sc length:4];
        [annexB appendBytes:baseAddr + offset length:naluLen];
        offset += naluLen;
    }

    _bytesEncoded += annexB.length;
    rdp_debug("encoded %zu bytes (key=%d frame=%lld)", annexB.length, isKey, _frameIndex);

    FrameEncoderOutputBlock handler = self.outputHandler;
    if (handler) handler((const uint8_t *)annexB.bytes, annexB.length, isKey,
                         dx, dy, dw, dh);
}

@end

static void vt_callback(void *outputCallbackRefCon, void *sourceFrameRefCon,
                         OSStatus status, VTEncodeInfoFlags infoFlags,
                         CMSampleBufferRef sampleBuffer) {
    (void)infoFlags;
    if (status != noErr) {
        rdp_error("VT encode callback error: %d", (int)status);
        return;
    }
    if (!sampleBuffer) return;
    [(__bridge FrameEncoder *)outputCallbackRefCon handleSampleBuffer:sampleBuffer
                                                            dirtyRect:sourceFrameRefCon];
}
