#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

@class RDPSession;

@protocol RDPSessionDelegate <NSObject>
- (void)sessionDidEnd:(RDPSession *)session error:(nullable NSError *)error;
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
- (void)disconnect;

@end

NS_ASSUME_NONNULL_END
