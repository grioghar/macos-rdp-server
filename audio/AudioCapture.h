#pragma once
#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>

NS_ASSUME_NONNULL_BEGIN

/* Delivers interleaved signed 16-bit stereo PCM at 48 kHz. */
typedef void (^AudioCaptureBlock)(const int16_t *samples, uint32_t frameCount);

@interface AudioCapture : NSObject

@property (nonatomic, copy, nullable) AudioCaptureBlock captureBlock;
@property (nonatomic, readonly) BOOL isCapturing;

- (BOOL)startWithError:(NSError **)error;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
