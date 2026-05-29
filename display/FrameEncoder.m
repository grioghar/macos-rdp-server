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

- (void)encodeFrame:(IOSurfaceRef)surface {
    if (!_session || !surface) return;
    CVPixelBufferRef pixbuf = NULL;
    CVReturn cvErr = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault,
                                                      surface, NULL, &pixbuf);
    if (cvErr != kCVReturnSuccess || !pixbuf) {
        rdp_error("CVPixelBufferCreateWithIOSurface failed: %d", cvErr);
        return;
    }
    CMTime pts = CMTimeMake(_frameIndex++, 60);
    VTCompressionSessionEncodeFrame(_session, pixbuf, pts, kCMTimeInvalid,
                                    NULL, NULL, NULL);
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

- (void)handleSampleBuffer:(CMSampleBufferRef)buf {
    if (!CMSampleBufferDataIsReady(buf)) return;

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
    if (handler) handler((const uint8_t *)annexB.bytes, annexB.length, isKey);
}

@end

static void vt_callback(void *outputCallbackRefCon, void *sourceFrameRefCon,
                         OSStatus status, VTEncodeInfoFlags infoFlags,
                         CMSampleBufferRef sampleBuffer) {
    (void)sourceFrameRefCon; (void)infoFlags;
    if (status != noErr) {
        rdp_error("VT encode callback error: %d", (int)status);
        return;
    }
    if (!sampleBuffer) return;
    [(__bridge FrameEncoder *)outputCallbackRefCon handleSampleBuffer:sampleBuffer];
}
