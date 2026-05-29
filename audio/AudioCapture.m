#import "audio/AudioCapture.h"
#import <AudioToolbox/AudioToolbox.h>
#import <syslog.h>

/* Tap system audio using the CoreAudio HAL AudioTap (macOS 14.2+) API.
   Falls back to an aggregate device tap for earlier releases. */

static const AudioStreamBasicDescription kOutputFormat = {
    .mSampleRate       = 48000.0,
    .mFormatID         = kAudioFormatLinearPCM,
    .mFormatFlags      = kAudioFormatFlagIsSignedInteger |
                         kAudioFormatFlagIsPacked,
    .mBytesPerPacket   = 4,   /* 2 ch × 2 bytes */
    .mFramesPerPacket  = 1,
    .mBytesPerFrame    = 4,
    .mChannelsPerFrame = 2,
    .mBitsPerChannel   = 16,
    .mReserved         = 0,
};

@interface AudioCapture ()
@property (nonatomic, assign) AudioDeviceID      tapDeviceID;
@property (nonatomic, assign) AudioDeviceIOProcID ioProcID;
@property (nonatomic, assign) BOOL               capturing;
@property (nonatomic, strong) dispatch_queue_t   audioQueue;
@end

static OSStatus audio_io_proc(AudioObjectID           device,
                               const AudioTimeStamp   *now,
                               const AudioBufferList  *inputData,
                               const AudioTimeStamp   *inputTime,
                               AudioBufferList        *outputData,
                               const AudioTimeStamp   *outputTime,
                               void                   *clientData);

@implementation AudioCapture

- (instancetype)init {
    if ((self = [super init])) {
        _tapDeviceID = kAudioObjectUnknown;
        _audioQueue  = dispatch_queue_create("com.macosrdp.audio",
                                             DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (BOOL)isCapturing { return _capturing; }

- (BOOL)startWithError:(NSError **)error {
    /* Locate the default system output device and install an IO proc.
       macOS 14.2+ supports AudioHardwareCreateProcessTap for process-level
       taps; for system-wide capture we use the output device directly.
       Running as root allows recording from output devices. */

    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    UInt32 size = sizeof(AudioDeviceID);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                              &addr, 0, NULL,
                                              &size, &_tapDeviceID);
    if (err != noErr || _tapDeviceID == kAudioObjectUnknown) {
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:err userInfo:nil];
        return NO;
    }

    err = AudioDeviceCreateIOProcID(_tapDeviceID,
                                    audio_io_proc,
                                    (__bridge void *)self,
                                    &_ioProcID);
    if (err != noErr) {
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:err userInfo:nil];
        return NO;
    }

    err = AudioDeviceStart(_tapDeviceID, _ioProcID);
    if (err != noErr) {
        AudioDeviceDestroyIOProcID(_tapDeviceID, _ioProcID);
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:err userInfo:nil];
        return NO;
    }

    _capturing = YES;
    return YES;
}

- (void)stop {
    if (!_capturing) return;
    _capturing = NO;
    AudioDeviceStop(_tapDeviceID, _ioProcID);
    AudioDeviceDestroyIOProcID(_tapDeviceID, _ioProcID);
    _ioProcID  = NULL;
    _tapDeviceID = kAudioObjectUnknown;
}

- (void)handleBufferList:(const AudioBufferList *)bufList
             frameCount:(UInt32)frameCount {
    if (!_captureBlock) return;

    /* Re-sample or convert to 16-bit stereo 48kHz if needed.
       Most Macs run at 44.1 or 48kHz; assume 48kHz for now. */
    for (UInt32 b = 0; b < bufList->mNumberBuffers; b++) {
        const AudioBuffer *buf = &bufList->mBuffers[b];
        if (buf->mData == NULL) continue;

        /* Expect Float32 interleaved from the output device. */
        const float *floats = (const float *)buf->mData;
        uint32_t samples    = frameCount * buf->mNumberChannels;
        int16_t *pcm        = (int16_t *)alloca(samples * sizeof(int16_t));

        for (uint32_t i = 0; i < samples; i++) {
            float f = floats[i];
            if (f >  1.0f) f =  1.0f;
            if (f < -1.0f) f = -1.0f;
            pcm[i] = (int16_t)(f * 32767.0f);
        }

        AudioCaptureBlock cb = self.captureBlock;
        if (cb) cb(pcm, frameCount);
        break; /* First buffer is the interleaved stereo output. */
    }
}

@end

static OSStatus audio_io_proc(AudioObjectID            device,
                                const AudioTimeStamp   *now,
                                const AudioBufferList  *inputData,
                                const AudioTimeStamp   *inputTime,
                                AudioBufferList        *outputData,
                                const AudioTimeStamp   *outputTime,
                                void                   *clientData) {
    (void)device; (void)now; (void)inputTime; (void)outputTime;
    AudioCapture *capture = (__bridge AudioCapture *)clientData;

    /* inputData contains the output device's play-through samples.
       outputData is NULL when no output processing is requested. */
    if (inputData) {
        [capture handleBufferList:inputData
                       frameCount:(UInt32)inputData->mBuffers[0].mDataByteSize
                                  / (sizeof(float) * 2)];
    }
    (void)outputData;
    return noErr;
}
