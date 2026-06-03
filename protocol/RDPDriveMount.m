/* RDPDriveMount.m — macOS placeholder mount for RDPDR-redirected client drives.
 *
 * After the MS-RDPEFS handshake, the server knows which client drives exist (by
 * name and device id). This file implements the desktop-visible placeholder while
 * the full File Provider (NSFileProviderReplicatedExtension) mount is being built.
 *
 * Architecture decision: we use Apple's File Provider framework instead of FUSE
 * for drive mounting. File Provider requires no kext, no third-party kernel
 * extensions, and no brew dependencies — it is a pure Swift/Obj-C extension that
 * runs in-process and presents the Windows drive as a native ~/Desktop/RDP-Drives/
 * volume. IRP ops (Create, Read, Write, QueryDirectory, Close) are translated into
 * WTS channel PDUs inside the extension's NSFileProviderReplicatedExtension
 * implementation. See daemon/FileProvider/ for the in-progress implementation.
 *
 * Called from protocol/RDPPeer.c after DEVICE_LIST_ANNOUNCE under xportLock.
 * The function is deliberately synchronous and lightweight — it only touches the
 * filesystem and does no network I/O.
 */

#define RDP_LOG_COMPONENT "rdpdr"
#include "logging/RDPLog.h"
#include "protocol/RDPPeer.h"

#import <Foundation/Foundation.h>

#if __has_include(<FileProvider/FileProvider.h>)
#import "daemon/FileProvider/RDPFileProvider.h"
#endif

void rdp_drive_mount_placeholder(const char *driveName, uint32_t deviceId) {
    if (!driveName || !*driveName) return;

    @autoreleasepool {
        NSString *name = [NSString stringWithUTF8String:driveName];

        /* Locate ~/Desktop/RDP-Drives/<driveName>/ */
        NSFileManager *fm = [NSFileManager defaultManager];
        NSURL *desktop = [[fm URLsForDirectory:NSDesktopDirectory
                                     inDomains:NSUserDomainMask] firstObject];
        if (!desktop) {
            rdp_verbose("rdpdr: cannot locate Desktop directory — skipping placeholder");
            return;
        }

        NSURL *rdpDir  = [desktop URLByAppendingPathComponent:@"RDP-Drives"
                                                  isDirectory:YES];
        NSURL *driveDir = [rdpDir URLByAppendingPathComponent:name
                                                   isDirectory:YES];

        /* Create the directory tree if needed. */
        NSError *err = nil;
        if (![fm createDirectoryAtURL:driveDir
          withIntermediateDirectories:YES
                           attributes:nil
                                error:&err]) {
            rdp_verbose("rdpdr: failed to create placeholder dir %s: %s",
                        driveDir.path.UTF8String,
                        err.localizedDescription.UTF8String);
            return;
        }

        /* Write / overwrite the info file every connection so the message stays
         * current. */
        NSURL *infoFile = [driveDir URLByAppendingPathComponent:@"README.txt"];
        NSString *info = [NSString stringWithFormat:
            @"RDP Drive Redirection — %@\n"
             "\n"
             "The Windows drive \"%@\" is redirected via MS-RDPEFS.\n"
             "\n"
             "Access it at: /Volumes/RDP-%@ (mounted via WebDAV)\n"
             "  Open Finder → Go → /Volumes/RDP-%@, or browse it in Finder's\n"
             "  sidebar under Network. Read and write work; the WebDAV bridge\n"
             "  translates every file op into an IRP sent to the Windows client.\n"
             "\n"
             "This Desktop folder is just a marker — use /Volumes/RDP-%@ instead.\n",
            name, name, name.uppercaseString, name.uppercaseString, name.uppercaseString];

        NSError *writeErr = nil;
        if (![info writeToURL:infoFile
                   atomically:YES
                     encoding:NSUTF8StringEncoding
                        error:&writeErr]) {
            rdp_verbose("rdpdr: could not write README for drive %s: %s",
                        driveName, writeErr.localizedDescription.UTF8String);
        }

        rdp_info("rdpdr: placeholder created at %s (File Provider mount in progress)",
                 driveDir.path.UTF8String);

        /* Register an NSFileProviderDomain for this drive so Finder can expose
         * it as a virtual volume.  The actual I/O (IRP forwarding) is wired up
         * in a later phase; for now this just announces the domain. */
#if __has_include(<FileProvider/FileProvider.h>)
        [RDPFileProvider mountDrive:name deviceId:deviceId];
#endif
    }
}
