#import "daemon/Authenticator.h"
#import <OpenDirectory/OpenDirectory.h>
#import <Foundation/Foundation.h>
#define RDP_LOG_COMPONENT "auth"
#include "logging/RDPLog.h"

@implementation Authenticator

+ (BOOL)authenticationDisabled {
    const char *env = getenv("RDP_ALLOW_NO_AUTH");
    return (env && strcmp(env, "1") == 0);
}

/* Primary path: ask OpenDirectory for the user's record on the local node and
 * call -verifyPassword:. Returns YES on a confirmed match, NO on a confirmed
 * mismatch / unknown user. *odUsable is set NO if OD itself could not be reached
 * (no node, lookup error) so the caller can fall back to dscl. */
+ (BOOL)verifyWithOpenDirectory:(NSString *)username
                       password:(NSString *)password
                       odUsable:(BOOL *)odUsable {
    *odUsable = NO;
    @autoreleasepool {
        NSError *err = nil;
        ODSession *session = [ODSession defaultSession];
        if (!session) {
            rdp_verbose("auth: no default ODSession");
            return NO;
        }
        /* Local node holds the Mac's own accounts (no directory server needed). */
        ODNode *node = [ODNode nodeWithSession:session
                                          name:@"/Local/Default"
                                         error:&err];
        if (!node) {
            rdp_verbose("auth: could not open /Local/Default node: %s",
                        err.localizedDescription.UTF8String);
            return NO;
        }
        ODRecord *record = [node recordWithRecordType:kODRecordTypeUsers
                                                 name:username
                                           attributes:nil
                                                error:&err];
        if (!record) {
            /* Reaching the node but finding no record = OD is usable and the
             * user simply doesn't exist → authoritative DENY, no fallback. */
            *odUsable = YES;
            rdp_info("auth: user '%s' not found in local directory",
                     username.UTF8String);
            return NO;
        }

        *odUsable = YES;
        err = nil;
        BOOL ok = [record verifyPassword:password error:&err];
        if (!ok) {
            rdp_info("auth: password verification FAILED for '%s'%s",
                     username.UTF8String,
                     err ? "" : " (wrong password)");
            if (err)
                rdp_verbose("auth: verifyPassword error: %s",
                            err.localizedDescription.UTF8String);
        }
        return ok;
    }
}

/* Fallback path: `/usr/bin/dscl . -authonly <user> <pass>` exits 0 on success.
 * Used only when OpenDirectory could not be reached at all. The password is
 * passed as an argv element (visible only to root in this root daemon); it is
 * never logged. */
+ (BOOL)verifyWithDscl:(NSString *)username password:(NSString *)password {
    @autoreleasepool {
        NSTask *task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/dscl"];
        task.arguments = @[@".", @"-authonly", username, password];
        task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
        task.standardError  = [NSFileHandle fileHandleWithNullDevice];
        NSError *err = nil;
        if (![task launchAndReturnError:&err]) {
            rdp_error("auth: failed to launch dscl: %s",
                      err.localizedDescription.UTF8String);
            return NO;
        }
        [task waitUntilExit];
        BOOL ok = (task.terminationStatus == 0);
        rdp_info("auth: dscl fallback for '%s' -> %s",
                 username.UTF8String, ok ? "OK" : "DENIED");
        return ok;
    }
}

+ (BOOL)validateUsername:(NSString *)username
                password:(NSString *)password
                  domain:(NSString *)domain {
    if ([self authenticationDisabled]) {
        rdp_info("auth: RDP_ALLOW_NO_AUTH=1 — skipping credential check");
        return YES;
    }

    if (username.length == 0 || password.length == 0) {
        rdp_info("auth: missing %s — rejecting (fail-closed)",
                 username.length == 0 ? "username" : "password");
        return NO;
    }

    rdp_info("auth: validating user='%s' domain='%s'",
             username.UTF8String, domain.length ? domain.UTF8String : "(none)");

    BOOL odUsable = NO;
    BOOL ok = [self verifyWithOpenDirectory:username
                                   password:password
                                   odUsable:&odUsable];
    if (odUsable) {
        rdp_info("auth: OpenDirectory result for '%s' -> %s",
                 username.UTF8String, ok ? "GRANTED" : "DENIED");
        return ok;
    }

    rdp_verbose("auth: OpenDirectory unavailable — falling back to dscl");
    ok = [self verifyWithDscl:username password:password];
    rdp_info("auth: final result for '%s' -> %s",
             username.UTF8String, ok ? "GRANTED" : "DENIED");
    return ok;
}

@end
