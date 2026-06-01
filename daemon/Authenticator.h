#pragma once
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/* Validates RDP logon credentials against the local macOS account database.
 *
 * Authentication is FAIL-CLOSED: an empty username/password, an unknown account,
 * or a wrong password all return NO. Set the environment variable
 * RDP_ALLOW_NO_AUTH=1 to bypass the check entirely (escape hatch in case
 * OpenDirectory validation misbehaves on a given box) — in that mode every
 * connection, including one with no credentials, is allowed.
 *
 * NEVER logs the password. */
@interface Authenticator : NSObject

/* YES if RDP_ALLOW_NO_AUTH=1 — authentication is disabled and all connections
 * are permitted without a credential check. */
+ (BOOL)authenticationDisabled;

/* Validate username + password against the local OpenDirectory node (falls back
 * to `/usr/bin/dscl . -authonly` if OD is unavailable). Returns YES only on a
 * confirmed match. The domain is accepted for logging/future use but local Mac
 * accounts are not domain-qualified, so it is otherwise ignored. */
+ (BOOL)validateUsername:(nullable NSString *)username
                password:(nullable NSString *)password
                  domain:(nullable NSString *)domain;

@end

NS_ASSUME_NONNULL_END
