#import "audio/AudioCapture.h"
#import <AudioToolbox/AudioToolbox.h>
#define RDP_LOG_COMPONENT "audio"
#include "logging/RDPLog.h"

@interface AudioCapture ()
@property (nonatomic, assign) AudioDeviceID      tapDeviceID;
@property (nonatomic, assign) AudioDeviceIOProcID ioProcID;
@property (nonatomic, assign) BOOL               capturing;
@property (nonatomic, assign) uint64_t           framesCaptured;
@end

static OSStatus audio_io_proc(AudioObjectID device, const AudioTimeStamp *now,
                               const AudioBufferList *inputData,
                               const AudioTimeStamp *inputTime,
                               AudioBufferList *outputData,
                               const AudioTimeStamp *outputTime, void *clientData);

@implementation AudioCapture

- (instancetype)init {
    if ((self = [super init])) { _tapDeviceID = kAudioObjectUnknown; }
    return self;
}

- (BOOL)isCapturing { return _capturing; }

- (BOOL)startWithError:(NSError **)error {
    rdp_verbose("locating default output device for audio tap");

    AudioObjectPropertyAddress addr = {
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    UInt32 size = sizeof(AudioDeviceID);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                              0, NULL, &size, &_tapDeviceID);
    if (err != noErr || _tapDeviceID == kAudioObjectUnknown) {
        rdp_error("could not find default output device: %d", (int)err);
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:err userInfo:nil];
        return NO;
    }
    rdp_verbose("default output device ID: %u", _tapDeviceID);

    err = AudioDeviceCreateIOProcID(_tapDeviceID, audio_io_proc,
                                    (__bridge void *)self, &_ioProcID);
    if (err != noErr) {
        rdp_error("AudioDeviceCreateIOProcID failed: %d", (int)err);
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:err userInfo:nil];
        return NO;
    }

    err = AudioDeviceStart(_tapDeviceID, _ioProcID);
    if (err != noErr) {
        rdp_error("AudioDeviceStart failed: %d (check Microphone permission)", (int)err);
        AudioDeviceDestroyIOProcID(_tapDeviceID, _ioProcID);
        if (error) *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                               code:err userInfo:nil];
        return NO;
    }

    _capturing = YES;
    rdp_info("audio capture started on device %u", _tapDeviceID);
    return YES;
}

- (void)stop {
    if (!_capturing) return;
    rdp_verbose("stopping audio capture after %llu frames", _framesCaptured);
    _capturing = NO;
    AudioDeviceStop(_tapDeviceID, _ioProcID);
    AudioDeviceDestroyIOProcID(_tapDeviceID, _ioProcID);
    _ioProcID = NULL; _tapDeviceID = kAudioObjectUnknown;
}

- (void)handleBufferList:(const AudioBufferList *)bufList frameCount:(UInt32)frameCount {
    if (!_captureBlock) return;
    _framesCaptured += frameCount;
    if (_framesCaptured % 48000 == 0)   /* log roughly every second */
        rdp_verbose("audio: %llu frames captured", _framesCaptured);

    for (UInt32 b = 0; b < bufList->mNumberBuffers; b++) {
        const AudioBuffer *buf = &bufList->mBuffers[b];
        if (!buf->mData) continue;
        const float *floats = (const float *)buf->mData;
        uint32_t samples    = frameCount * buf->mNumberChannels;
        int16_t *pcm        = (int16_t *)alloca(samples * sizeof(int16_t));
        for (uint32_t i = 0; i < samples; i++) {
            float f = floats[i];
            if (f >  1.0f) f =  1.0f;
            if (f < -1.0f) f = -1.0f;
            pcm[i] = (int16_t)(f * 32767.0f);
        }
        rdp_debug("audio buffer: %u frames %u ch", frameCount, buf->mNumberChannels);
        AudioCaptureBlock cb = self.captureBlock;
        if (cb) cb(pcm, frameCount);
        break;
    }
}

@end

static OSStatus audio_io_proc(AudioObjectID device, const AudioTimeStamp *now,
                               const AudioBufferList *inputData,
                               const AudioTimeStamp *inputTime,
                               AudioBufferList *outputData,
                               const AudioTimeStamp *outputTime, void *clientData) {
    (void)device; (void)now; (void)inputTime; (void)outputTime; (void)outputData;
    AudioCapture *capture = (__bridge AudioCapture *)clientData;
    if (inputData) {
        UInt32 frameCount = inputData->mBuffers[0].mDataByteSize / (sizeof(float) * 2);
        [capture handleBufferList:inputData frameCount:frameCount];
    }
    return noErr;
}
