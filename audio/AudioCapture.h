#pragma once
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>

NS_ASSUME_NONNULL_BEGIN

/* Delivers interleaved signed 16-bit stereo PCM at the configured output rate
 * (see outputSampleRate; defaults to 48 kHz). */
typedef void (^AudioCaptureBlock)(const int16_t *samples, uint32_t frameCount);

@interface AudioCapture : NSObject

@property (nonatomic, copy, nullable) AudioCaptureBlock captureBlock;
@property (nonatomic, readonly) BOOL isCapturing;

/* Target output sample rate in Hz for the PCM delivered to captureBlock. The
 * tap is resampled from its native rate to this rate. MUST equal the rate the
 * RDP client actually plays at (rdpsnd does not resample), or the audio is
 * pitch-shifted. Defaults to 48000. Updated by the session once rdpsnd format
 * negotiation completes. Read/written atomically; safe to set from another
 * thread while capturing (the IO proc picks up the new value on its next
 * callback). 0 is ignored (keeps the current rate). */
@property (nonatomic, assign) uint32_t outputSampleRate;

- (BOOL)startWithError:(NSError **)error;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
