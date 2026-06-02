/* RDPFileProvider.m — NSFileProviderManager wrapper for RDPDR drive mounting.
 *
 * Skeleton implementation: registers / removes NSFileProviderDomain objects.
 * Full IRP forwarding (NSFileProviderReplicatedExtension) is a future phase.
 *
 * Compiled only when <FileProvider/FileProvider.h> is present (macOS 12+).
 */

#if __has_include(<FileProvider/FileProvider.h>)

#define RDP_LOG_COMPONENT "fileprovider"
#include "logging/RDPLog.h"

#import "RDPFileProvider.h"
#import <FileProvider/FileProvider.h>

@implementation RDPFileProvider

/* ── domain identifier helper ───────────────────────────────────────────── */

+ (NSFileProviderDomainIdentifier)_identifierForDrive:(NSString *)driveName {
    /* Use a stable, lowercase, prefixed string so it survives reconnects. */
    NSString *raw = [NSString stringWithFormat:@"rdp.%@", driveName.lowercaseString];
    return (NSFileProviderDomainIdentifier)raw;
}

/* ── mount ──────────────────────────────────────────────────────────────── */

+ (void)mountDrive:(NSString *)driveName deviceId:(uint32_t)deviceId {
    if (!driveName.length) return;

    NSFileProviderDomainIdentifier domainId = [self _identifierForDrive:driveName];

    /* Display name shown to the user in Finder's sidebar. */
    NSString *displayName = [NSString stringWithFormat:@"RDP Drive %@", driveName.uppercaseString];

    NSFileProviderDomain *domain =
        [[NSFileProviderDomain alloc] initWithIdentifier:domainId
                                             displayName:displayName];

    rdp_info("fileprovider: registering domain \"%s\" for drive %s (deviceId=%u)",
             domainId.UTF8String,
             driveName.UTF8String,
             (unsigned)deviceId);

    [NSFileProviderManager addDomain:domain completionHandler:^(NSError * _Nullable error) {
        if (error) {
            /* NSFileProviderErrorDomainAlreadyMounted (-2008) is benign —
             * another session already added this drive. Log at verbose level. */
            if (error.code == NSFileProviderErrorDomainAlreadyMounted) {
                rdp_verbose("fileprovider: domain \"%s\" already mounted — reusing",
                            domainId.UTF8String);
            } else {
                rdp_info("fileprovider: addDomain \"%s\" failed: %s",
                         domainId.UTF8String,
                         error.localizedDescription.UTF8String);
            }
        } else {
            rdp_info("fileprovider: domain \"%s\" registered successfully",
                     domainId.UTF8String);
        }
    }];
}

/* ── unmount ────────────────────────────────────────────────────────────── */

+ (void)unmountDrive:(NSString *)driveName {
    if (!driveName.length) return;

    NSFileProviderDomainIdentifier domainId = [self _identifierForDrive:driveName];

    NSFileProviderDomain *domain =
        [[NSFileProviderDomain alloc] initWithIdentifier:domainId
                                             displayName:driveName];

    rdp_info("fileprovider: removing domain \"%s\" for drive %s",
             domainId.UTF8String,
             driveName.UTF8String);

    [NSFileProviderManager removeDomain:domain completionHandler:^(NSError * _Nullable error) {
        if (error) {
            /* Domain not found is benign when the drive was never mounted. */
            rdp_verbose("fileprovider: removeDomain \"%s\" error: %s",
                        domainId.UTF8String,
                        error.localizedDescription.UTF8String);
        } else {
            rdp_info("fileprovider: domain \"%s\" removed", domainId.UTF8String);
        }
    }];
}

@end

#endif /* __has_include(<FileProvider/FileProvider.h>) */
