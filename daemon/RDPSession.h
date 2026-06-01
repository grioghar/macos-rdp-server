#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

@class RDPSession;

@protocol RDPSessionDelegate <NSObject>
- (void)sessionDidEnd:(RDPSession *)session error:(nullable NSError *)error;

/* Called (on the session's own queue) once a session has authenticated its
 * client and is about to bring up its virtual display. The delegate (RDPServer)
 * makes this the single active session, superseding any currently-active one:
 * it signals the old session to disconnect and BLOCKS until the old session has
 * released its display, then returns YES so the caller may proceed. Returns NO
 * if this session should abort (e.g. it was itself superseded while waiting, or
 * the server is shutting down). */
- (BOOL)sessionDidAuthenticateAndRequestActivation:(RDPSession *)session;
@end

typedef NS_ENUM(NSInteger, RDPSessionState) {
    RDPSessionStateConnecting,
    RDPSessionStateNegotiating,
    RDPSessionStateActive,
    RDPSessionStateDisconnecting,
    RDPSessionStateDisconnected,
};

@interface RDPSession : NSObject

@property (nonatomic, readonly) int clientFd;
@property (nonatomic, readonly) RDPSessionState state;
@property (nonatomic, readonly) NSString *clientAddress;
@property (nonatomic, weak, nullable) id<RDPSessionDelegate> delegate;

- (instancetype)initWithFileDescriptor:(int)fd clientAddress:(NSString *)address;
- (void)start;

/* Request that this session disconnect. Sets the state to Disconnecting so the
 * peer run loop exits and teardown runs (releasing the virtual display + display
 * assertions). Safe to call from any thread; returns immediately. */
- (void)disconnect;

/* Block the calling thread until this session's teardown has fully completed
 * (virtual display released, peer destroyed). Returns YES if teardown finished
 * within `timeout` seconds, NO on timeout. Used by the takeover path so a new
 * session does not create its virtual display until the superseded one has
 * released the display. MUST NOT be called from the session's own queue (that
 * would deadlock) — call it from the superseding session's queue. */
- (BOOL)waitForTeardown:(NSTimeInterval)timeout;

@end

NS_ASSUME_NONNULL_END
