#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^ClipboardSendBlock)(const uint8_t *data, size_t len, uint32_t format);

/* CF_TEXT=1, CF_UNICODETEXT=13 — matches Windows clipboard format IDs */
#define RDP_CB_FORMAT_TEXT        1
#define RDP_CB_FORMAT_UNICODETEXT 13
#define RDP_CB_FORMAT_DIB         8
#define RDP_CB_FORMAT_PNG         0xC004

@interface ClipboardSync : NSObject

@property (nonatomic, copy, nullable) ClipboardSendBlock sendToClientBlock;

- (void)start;
- (void)stop;

/* Called by RDPSession when client sends clipboard data. */
- (void)receiveFromClient:(const uint8_t *)data length:(size_t)len
                   format:(uint32_t)format;

@end

NS_ASSUME_NONNULL_END
