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

void rdp_drive_mount_placeholder(const char *driveName) {
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
             "This folder is a placeholder for the Windows drive \"%@\" that your\n"
             "RDP client advertised via the MS-RDPEFS protocol.\n"
             "\n"
             "Current state: handshake complete; drive is enumerated.\n"
             "\n"
             "Full read/write mount: coming via Apple File Provider framework\n"
             "  (NSFileProviderReplicatedExtension) — no brew/macFUSE/kext required.\n"
             "  The File Provider extension forwards IRP ops (Create, Read, Write,\n"
             "  QueryDirectory, Close) over the rdpdr WTS channel.\n"
             "\n"
             "For now you can use the RDP session's clipboard to transfer files.\n",
            name, name];

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
    }
}
