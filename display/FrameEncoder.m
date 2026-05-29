#import "display/FrameEncoder.h"
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <IOSurface/IOSurface.h>
#import <syslog.h>

/* VideoToolbox H.264 real-time encoder.
   Produces Annex-B bytestream suitable for RDPGFX AVC420. */

@interface FrameEncoder ()
@property (nonatomic, assign) VTCompressionSessionRef session;
@property (nonatomic, assign) uint32_t w;
@property (nonatomic, assign) uint32_t h;
@property (nonatomic, assign) uint32_t bitrateKbps;
@property (nonatomic, assign) int64_t  frameIndex;
@end

static void vt_callback(void *outputCallbackRefCon,
                         void *sourceFrameRefCon,
                         OSStatus status,
                         VTEncodeInfoFlags infoFlags,
                         CMSampleBufferRef sampleBuffer);

@implementation FrameEncoder

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                      bitrate:(uint32_t)bitrateKbps {
    if ((self = [super init])) {
        _w           = width;
        _h           = height;
        _bitrateKbps = bitrateKbps;
    }
    return self;
}

- (uint32_t)width  { return _w; }
- (uint32_t)height { return _h; }

- (BOOL)start {
    OSStatus err = VTCompressionSessionCreate(
        kCFAllocatorDefault,
        (int32_t)_w, (int32_t)_h,
        kCMVideoCodecType_H264,
        NULL, /* encoderSpecification — let VT choose */
        NULL, /* sourceImageBufferAttributes */
        NULL, /* compressedDataAllocator */
        vt_callback,
        (__bridge void *)self,
        &_session
    );
    if (err != noErr) {
        syslog(LOG_ERR, "VTCompressionSessionCreate failed: %d", (int)err);
        return NO;
    }

    int32_t bitrate = (int32_t)(_bitrateKbps * 1000);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_AverageBitRate,
                         (__bridge CFTypeRef)@(bitrate));
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_RealTime,
                         kCFBooleanTrue);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_Baseline_AutoLevel);
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_H264EntropyMode,
                         kVTH264EntropyMode_CAVLC);
    /* Request periodic keyframes for recovery after packet loss. */
    VTSessionSetProperty(_session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                         (__bridge CFTypeRef)@(120));

    VTCompressionSessionPrepareToEncodeFrames(_session);
    return YES;
}

- (void)encodeFrame:(IOSurfaceRef)surface {
    if (!_session || !surface) return;

    /* Wrap the IOSurface in a CVPixelBuffer without copying. */
    CVPixelBufferRef pixbuf = NULL;
    CVReturn cvErr = CVPixelBufferCreateWithIOSurface(
        kCFAllocatorDefault, surface, NULL, &pixbuf);
    if (cvErr != kCVReturnSuccess || !pixbuf) return;

    CMTime pts = CMTimeMake(_frameIndex++, 60);
    VTCompressionSessionEncodeFrame(
        _session, pixbuf, pts, kCMTimeInvalid, NULL, NULL, NULL);
    CVPixelBufferRelease(pixbuf);
}

- (void)stop {
    if (_session) {
        VTCompressionSessionCompleteFrames(_session, kCMTimeIndefinite);
        VTCompressionSessionInvalidate(_session);
        CFRelease(_session);
        _session = NULL;
    }
}

- (void)handleSampleBuffer:(CMSampleBufferRef)buf {
    if (!CMSampleBufferDataIsReady(buf)) return;

    BOOL isKey = NO;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buf, FALSE);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        isKey = !CFDictionaryContainsKey(dict,
                    kCMSampleAttachmentKey_NotSync);
    }

    /* Convert CMSampleBuffer to Annex-B bytestream. */
    NSMutableData *annexB = [NSMutableData data];

    /* Prepend SPS/PPS parameter sets on key frames. */
    if (isKey) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(buf);
        size_t count = 0;
        CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, 0, NULL, NULL, &count, NULL);

        for (size_t i = 0; i < count; i++) {
            const uint8_t *ps; size_t psLen; int naluHeaderLen;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                fmt, i, &ps, &psLen, NULL, &naluHeaderLen);
            uint8_t startCode[4] = {0,0,0,1};
            [annexB appendBytes:startCode length:4];
            [annexB appendBytes:ps length:psLen];
        }
    }

    /* Append each NALU with Annex-B start code. */
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(buf);
    size_t offset = 0, totalLen = 0;
    char *baseAddr = NULL;
    CMBlockBufferGetDataPointer(block, 0, NULL, &totalLen, &baseAddr);

    while (offset < totalLen) {
        /* AVCC length prefix is 4 bytes big-endian. */
        uint32_t naluLen;
        memcpy(&naluLen, baseAddr + offset, 4);
        naluLen = CFSwapInt32BigToHost(naluLen);
        offset += 4;

        uint8_t startCode[4] = {0,0,0,1};
        [annexB appendBytes:startCode length:4];
        [annexB appendBytes:baseAddr + offset length:naluLen];
        offset += naluLen;
    }

    FrameEncoderOutputBlock handler = self.outputHandler;
    if (handler) {
        handler((const uint8_t *)annexB.bytes, annexB.length, isKey);
    }
}

@end

static void vt_callback(void *outputCallbackRefCon,
                         void *sourceFrameRefCon,
                         OSStatus status,
                         VTEncodeInfoFlags infoFlags,
                         CMSampleBufferRef sampleBuffer) {
    (void)sourceFrameRefCon; (void)infoFlags;
    if (status != noErr || !sampleBuffer) return;
    FrameEncoder *enc = (__bridge FrameEncoder *)outputCallbackRefCon;
    [enc handleSampleBuffer:sampleBuffer];
}
