#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@class RDPSession;

@protocol RDPServerDelegate <NSObject>
- (void)serverDidAcceptSession:(RDPSession *)session;
- (void)serverSession:(RDPSession *)session didEndWithError:(nullable NSError *)error;
@end

@interface RDPServer : NSObject

@property (nonatomic, weak, nullable) id<RDPServerDelegate> delegate;
@property (nonatomic, readonly) uint16_t port;
@property (nonatomic, readonly) BOOL isRunning;

- (instancetype)initWithPort:(uint16_t)port;
- (BOOL)startWithError:(NSError **)error;
- (void)stop;

/* YES if a client session is currently active (authenticated + owns the
 * display). Used by the auto-updater to defer a swap/restart while a user is
 * connected. Class-level so AutoUpdate need not hold an RDPServer reference;
 * backed by the single live server instance's active-session registry. */
+ (BOOL)hasActiveSession;

@end

NS_ASSUME_NONNULL_END
