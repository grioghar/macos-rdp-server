#import "audio/AudioCapture.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
#include <math.h>
#include <mach/mach_time.h>

/* macOS 14.4+ Core Audio process-tap API. These headers ship with the
 * 14.4+ SDK; the symbols are weak-linked and guarded with @available so the
 * binary still loads (and gracefully degrades) on older systems. */
#if __has_include(<CoreAudio/CATapDescription.h>)
#import <CoreAudio/CATapDescription.h>
#import <CoreAudio/AudioHardwareTapping.h>
#define RDP_HAVE_CATAP 1
#endif

#define RDP_LOG_COMPONENT "audio"
#include "logging/RDPLog.h"

#include <stdatomic.h>

/* Default output rate before rdpsnd negotiation tells us the client's actual
 * playback rate. The tap is resampled to whatever outputSampleRate currently
 * holds; this is just the starting value. */
static const uint32_t kDefaultOutRate = 48000;
static const uint32_t kRDPChannels    = 2;

static inline int16_t clampF32(float f) {
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int16_t)lrintf(f * 32767.0f);
}

@interface AudioCapture () {
    /* Target output rate, written by setOutputSampleRate: (possibly from
     * another thread) and read by the real-time IO proc. Atomic so the IO proc
     * sees a consistent value without taking a lock on the audio thread. */
    _Atomic uint32_t _outRate;

    /* Wall-clock delivery-rate diagnostic: input frames seen since the last log
     * and the host-time of that last log, so we can compare the REAL frames/sec
     * the IO proc delivers against the queried srcSampleRate (a mismatch there
     * would itself cause a pitch shift). */
    uint64_t _diagFrames;
    uint64_t _diagLastHostTime;
    double   _hostTicksToSeconds;
}
@property (nonatomic, assign) AudioObjectID       tapID;        /* process tap            */
@property (nonatomic, assign) AudioObjectID       aggID;        /* private aggregate dev  */
@property (nonatomic, assign) AudioDeviceIOProcID ioProcID;
@property (nonatomic, assign) BOOL                capturing;
@property (nonatomic, assign) uint64_t            framesCaptured;

/* Source format discovered from the aggregate's input stream. */
@property (nonatomic, assign) double              srcSampleRate;
@property (nonatomic, assign) uint32_t            srcChannels;

/* Linear-resampler carry state (previous tail sample per channel + phase). */
@property (nonatomic, assign) double              resamplePhase;
@property (nonatomic, assign) float               lastL;
@property (nonatomic, assign) float               lastR;
@property (nonatomic, assign) BOOL                haveLast;
@end

static OSStatus audio_io_proc(AudioObjectID device, const AudioTimeStamp *now,
                               const AudioBufferList *inputData,
                               const AudioTimeStamp *inputTime,
                               AudioBufferList *outputData,
                               const AudioTimeStamp *outputTime, void *clientData);

@implementation AudioCapture

- (instancetype)init {
    if ((self = [super init])) {
        _tapID = kAudioObjectUnknown;
        _aggID = kAudioObjectUnknown;
        atomic_store_explicit(&_outRate, kDefaultOutRate, memory_order_relaxed);
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        _hostTicksToSeconds = (double)tb.numer / (double)tb.denom / 1.0e9;
    }
    return self;
}

- (BOOL)isCapturing { return _capturing; }

- (uint32_t)outputSampleRate {
    return atomic_load_explicit(&_outRate, memory_order_relaxed);
}

- (void)setOutputSampleRate:(uint32_t)rate {
    if (rate == 0) return;  /* ignore: keep current rate */
    uint32_t prev = atomic_exchange_explicit(&_outRate, rate, memory_order_relaxed);
    if (prev != rate)
        rdp_info("audio output rate set to %u Hz (resampling tap %.0f Hz -> %u Hz)",
                 (unsigned)rate, _srcSampleRate, (unsigned)rate);
}

- (BOOL)failWithError:(NSError **)error code:(OSStatus)code msg:(const char *)msg {
    rdp_error("%s (status %d)", msg, (int)code);
    if (error) {
        *error = [NSError errorWithDomain:NSOSStatusErrorDomain
                                     code:(code ? code : -1)
                                 userInfo:@{ NSLocalizedDescriptionKey:
                                                 @(msg) }];
    }
    return NO;
}

- (BOOL)startWithError:(NSError **)error {
#if RDP_HAVE_CATAP
    if (@available(macOS 14.4, *)) {
        return [self startTapWithError:error];
    }
#endif
    rdp_error("system-audio process-tap requires macOS 14.4+; no audio will "
              "be captured");
    if (error) {
        *error = [NSError errorWithDomain:NSOSStatusErrorDomain code:-1
                                 userInfo:@{ NSLocalizedDescriptionKey:
                                 @"CoreAudio process-tap unavailable (needs macOS 14.4+)" }];
    }
    return NO;
}

#if RDP_HAVE_CATAP
- (BOOL)startTapWithError:(NSError **)error API_AVAILABLE(macos(14.4)) {
    OSStatus err;

    /* 1. Global stereo tap of all processes, private, locally unmuted so the
     *    user still hears their audio. */
    CATapDescription *desc =
        [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
    if (!desc) {
        return [self failWithError:error code:-1
                               msg:"CATapDescription alloc failed"];
    }
    desc.privateTap   = YES;
    /* Audio routing switch (RDP_AUDIO_LOCAL): "1"/unset (default) = CATapUnmuted,
     * so the desk user STILL hears audio locally while it also streams to the
     * remote client (audio in BOTH places). "0" = CATapMuted, so the tapped audio
     * is silenced on the Mac's speakers and only the remote client hears it
     * (e.g. for privacy / not disturbing people near the Mac). A GUI toggle writes
     * this env var into the LaunchAgent. */
    const char *localAudio = getenv("RDP_AUDIO_LOCAL");
    BOOL playLocally = !(localAudio && strcmp(localAudio, "0") == 0);
    desc.muteBehavior = playLocally ? CATapUnmuted : CATapMuted;
    desc.name         = @"macos-rdp system audio tap";
    rdp_info("audio routing: %s", playLocally ? "both (Mac + remote)" : "remote only (Mac muted)");

    /* 2. Create the process tap. */
    AudioObjectID tap = kAudioObjectUnknown;
    err = AudioHardwareCreateProcessTap(desc, &tap);
    if (err != noErr || tap == kAudioObjectUnknown) {
        return [self failWithError:error code:err
                               msg:"AudioHardwareCreateProcessTap failed "
                                   "(check TCC audio-recording permission)"];
    }
    _tapID = tap;

    /* 3. Get the tap UID. */
    CFStringRef tapUID = NULL;
    UInt32 sz = sizeof(tapUID);
    AudioObjectPropertyAddress uidAddr = {
        .mSelector = kAudioTapPropertyUID,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    err = AudioObjectGetPropertyData(_tapID, &uidAddr, 0, NULL, &sz, &tapUID);
    if (err != noErr || !tapUID) {
        [self stop];
        return [self failWithError:error code:err
                               msg:"could not read tap UID"];
    }
    NSString *tapUIDStr = (__bridge_transfer NSString *)tapUID;

    /* 4. Private aggregate device containing the tap. */
    NSString *aggUID = [[NSUUID UUID] UUIDString];
    NSDictionary *aggDict = @{
        @(kAudioAggregateDeviceNameKey):      @"macos-rdp tap aggregate",
        @(kAudioAggregateDeviceUIDKey):       aggUID,
        @(kAudioAggregateDeviceIsPrivateKey): @YES,
        @(kAudioAggregateDeviceTapAutoStartKey): @YES,
        @(kAudioAggregateDeviceTapListKey): @[
            @{
                @(kAudioSubTapUIDKey):              tapUIDStr,
                @(kAudioSubTapDriftCompensationKey): @YES,
            }
        ],
    };
    AudioObjectID agg = kAudioObjectUnknown;
    err = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggDict,
                                             &agg);
    if (err != noErr || agg == kAudioObjectUnknown) {
        [self stop];
        return [self failWithError:error code:err
                               msg:"AudioHardwareCreateAggregateDevice failed"];
    }
    _aggID = agg;

    /* 5. Query the aggregate's INPUT stream format (float32). */
    AudioStreamBasicDescription asbd = {0};
    sz = sizeof(asbd);
    AudioObjectPropertyAddress fmtAddr = {
        .mSelector = kAudioDevicePropertyStreamFormat,
        .mScope    = kAudioObjectPropertyScopeInput,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    err = AudioObjectGetPropertyData(_aggID, &fmtAddr, 0, NULL, &sz, &asbd);
    if (err != noErr || asbd.mSampleRate <= 0 || asbd.mChannelsPerFrame == 0) {
        [self stop];
        return [self failWithError:error code:err
                               msg:"could not read aggregate input format"];
    }
    _srcSampleRate = asbd.mSampleRate;
    _srcChannels   = asbd.mChannelsPerFrame;
    _resamplePhase = 0.0;
    _haveLast      = NO;
    _diagFrames       = 0;
    _diagLastHostTime = 0;
    rdp_info("tap input format: %.0f Hz, %u ch, %u bits, flags 0x%x",
             asbd.mSampleRate, (unsigned)asbd.mChannelsPerFrame,
             (unsigned)asbd.mBitsPerChannel, (unsigned)asbd.mFormatFlags);

    /* Cross-check: the device's nominal sample rate. If it disagrees with the
     * stream-format rate above, the IO proc may actually deliver frames at the
     * nominal rate — using the wrong _srcSampleRate in the resampler would
     * itself pitch-shift. Prefer the stream-format rate (that's the layout of
     * the bytes), but log the nominal so a mismatch is visible. */
    Float64 nominal = 0;
    UInt32 nsz = sizeof(nominal);
    AudioObjectPropertyAddress nomAddr = {
        .mSelector = kAudioDevicePropertyNominalSampleRate,
        .mScope    = kAudioObjectPropertyScopeGlobal,
        .mElement  = kAudioObjectPropertyElementMain,
    };
    if (AudioObjectGetPropertyData(_aggID, &nomAddr, 0, NULL, &nsz, &nominal) == noErr) {
        rdp_info("tap nominal sample rate: %.0f Hz", nominal);
        if (nominal > 0 && fabs(nominal - _srcSampleRate) > 1.0)
            rdp_info("WARNING: nominal rate %.0f Hz != stream-format rate %.0f Hz "
                     "— delivered frames/sec diagnostic will confirm the real rate",
                     nominal, _srcSampleRate);
    }

    uint32_t outRate = atomic_load_explicit(&_outRate, memory_order_relaxed);
    if (_srcSampleRate != (double)outRate)
        rdp_info("tap rate %.0f Hz != output %u Hz — linear-resampling",
                 _srcSampleRate, (unsigned)outRate);

    /* 6. Register IO proc + start. */
    err = AudioDeviceCreateIOProcID(_aggID, audio_io_proc,
                                    (__bridge void *)self, &_ioProcID);
    if (err != noErr || !_ioProcID) {
        [self stop];
        return [self failWithError:error code:err
                               msg:"AudioDeviceCreateIOProcID failed"];
    }
    err = AudioDeviceStart(_aggID, _ioProcID);
    if (err != noErr) {
        [self stop];
        return [self failWithError:error code:err
                               msg:"AudioDeviceStart failed"];
    }

    _capturing = YES;
    rdp_info("system-audio process-tap capture started (tap=%u agg=%u)",
             (unsigned)_tapID, (unsigned)_aggID);
    return YES;
}
#endif /* RDP_HAVE_CATAP */

- (void)stop {
#if RDP_HAVE_CATAP
    if (@available(macOS 14.4, *)) {
        if (_capturing) {
            rdp_verbose("stopping audio capture after %llu frames",
                        _framesCaptured);
        }
        _capturing = NO;
        if (_aggID != kAudioObjectUnknown && _ioProcID) {
            AudioDeviceStop(_aggID, _ioProcID);
            AudioDeviceDestroyIOProcID(_aggID, _ioProcID);
            _ioProcID = NULL;
        }
        if (_aggID != kAudioObjectUnknown) {
            AudioHardwareDestroyAggregateDevice(_aggID);
            _aggID = kAudioObjectUnknown;
        }
        if (_tapID != kAudioObjectUnknown) {
            AudioHardwareDestroyProcessTap(_tapID);
            _tapID = kAudioObjectUnknown;
        }
        return;
    }
#endif
    _capturing = NO;
}

/* Convert one IO-proc buffer (float32, _srcChannels, _srcSampleRate) into
 * interleaved int16 stereo at the current output rate (_outRate, == the rate
 * the RDP client plays at) and deliver via captureBlock. hostTime is the
 * buffer's mach host time, used only for the delivery-rate diagnostic. */
- (void)handleInput:(const AudioBufferList *)bufList hostTime:(uint64_t)hostTime {
    AudioCaptureBlock cb = self.captureBlock;
    if (!cb || !bufList || bufList->mNumberBuffers == 0) return;

    const AudioBuffer *buf = &bufList->mBuffers[0];
    if (!buf->mData) return;

    const uint32_t srcCh = _srcChannels ? _srcChannels : buf->mNumberChannels;
    if (srcCh == 0) return;
    const float *in = (const float *)buf->mData;
    /* Tap buffers are interleaved float32. */
    const uint32_t inFrames = buf->mDataByteSize / (sizeof(float) * srcCh);
    if (inFrames == 0) return;

    _framesCaptured += inFrames;

    /* Delivery-rate diagnostic: measure the REAL input frames/sec over
     * wall-clock and compare to the queried _srcSampleRate. A large gap means
     * the IO proc delivers at a different rate than we resample from, which
     * would pitch-shift. Logged ~every 5 s; cheap (no allocation). */
    if (hostTime && _hostTicksToSeconds > 0) {
        _diagFrames += inFrames;
        if (_diagLastHostTime == 0) {
            _diagLastHostTime = hostTime;
        } else {
            double elapsed = (double)(hostTime - _diagLastHostTime) *
                             _hostTicksToSeconds;
            if (elapsed >= 5.0) {
                double measured = (double)_diagFrames / elapsed;
                rdp_info("audio delivery: %.0f frames/sec measured "
                         "(queried src %.0f Hz, output %u Hz)",
                         measured, _srcSampleRate,
                         (unsigned)atomic_load_explicit(&_outRate,
                                                        memory_order_relaxed));
                _diagFrames       = 0;
                _diagLastHostTime = hostTime;
            }
        }
    }

    const double outRate = (double)atomic_load_explicit(&_outRate,
                                                        memory_order_relaxed);
    const double ratio = (_srcSampleRate > 0 ? outRate / _srcSampleRate : 1.0);

    /* Fast path: source already at the output rate — just downmix to stereo. */
    if (_srcSampleRate == outRate) {
        int16_t *pcm = (int16_t *)malloc((size_t)inFrames * kRDPChannels *
                                         sizeof(int16_t));
        if (!pcm) return;
        for (uint32_t f = 0; f < inFrames; f++) {
            float l, r;
            [self readFrame:in index:f channels:srcCh outL:&l outR:&r];
            pcm[f * 2 + 0] = clampF32(l);
            pcm[f * 2 + 1] = clampF32(r);
        }
        cb(pcm, inFrames);
        free(pcm);
        return;
    }

    /* Linear resample to output-rate stereo. Generate output frames by walking a
     * fractional read position across the input block, carrying the tail
     * sample between callbacks for continuity. */
    uint32_t maxOut = (uint32_t)((inFrames + 1) * ratio) + 2;
    int16_t *pcm = (int16_t *)malloc((size_t)maxOut * kRDPChannels *
                                     sizeof(int16_t));
    if (!pcm) return;

    double phase = _resamplePhase;      /* fractional input index in [0,inFrames) */
    float prevL = _lastL, prevR = _lastR;
    BOOL haveLast = _haveLast;
    uint32_t outCount = 0;

    while (phase < inFrames && outCount < maxOut) {
        uint32_t idx = (uint32_t)phase;
        double frac  = phase - (double)idx;

        float curL, curR;
        [self readFrame:in index:idx channels:srcCh outL:&curL outR:&curR];

        float pL, pR;
        if (idx == 0) {
            if (haveLast) { pL = prevL; pR = prevR; }
            else          { pL = curL; pR = curR; }
        } else {
            [self readFrame:in index:idx - 1 channels:srcCh outL:&pL outR:&pR];
        }

        float outL = (float)(pL + (curL - pL) * frac);
        float outR = (float)(pR + (curR - pR) * frac);
        pcm[outCount * 2 + 0] = clampF32(outL);
        pcm[outCount * 2 + 1] = clampF32(outR);
        outCount++;

        phase += 1.0 / ratio;
    }

    /* Carry phase remainder and the last input frame into the next callback. */
    [self readFrame:in index:inFrames - 1 channels:srcCh outL:&prevL outR:&prevR];
    _lastL = prevL; _lastR = prevR; _haveLast = YES;
    _resamplePhase = phase - (double)inFrames;
    if (_resamplePhase < 0) _resamplePhase = 0;

    if (outCount) cb(pcm, outCount);
    free(pcm);
}

/* Read one interleaved float frame, downmixing to a stereo (L,R) pair. */
- (void)readFrame:(const float *)in index:(uint32_t)f channels:(uint32_t)ch
             outL:(float *)outL outR:(float *)outR {
    const float *frame = in + (size_t)f * ch;
    if (ch == 1) {
        *outL = *outR = frame[0];
    } else if (ch == 2) {
        *outL = frame[0];
        *outR = frame[1];
    } else {
        /* >2 channels: take 0/1 as L/R (front pair). */
        *outL = frame[0];
        *outR = frame[1];
    }
}

@end

static OSStatus audio_io_proc(AudioObjectID device, const AudioTimeStamp *now,
                               const AudioBufferList *inputData,
                               const AudioTimeStamp *inputTime,
                               AudioBufferList *outputData,
                               const AudioTimeStamp *outputTime, void *clientData) {
    (void)device; (void)outputTime; (void)outputData;
    AudioCapture *capture = (__bridge AudioCapture *)clientData;
    if (capture && inputData) {
        /* Prefer the input timestamp's host time for the delivery-rate
         * diagnostic; fall back to `now` if the input clock is unset. */
        uint64_t hostTime = 0;
        if (inputTime && (inputTime->mFlags & kAudioTimeStampHostTimeValid))
            hostTime = inputTime->mHostTime;
        else if (now && (now->mFlags & kAudioTimeStampHostTimeValid))
            hostTime = now->mHostTime;
        else
            hostTime = mach_absolute_time();
        [capture handleInput:inputData hostTime:hostTime];
    }
    return noErr;
}
