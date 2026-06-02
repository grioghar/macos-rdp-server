/* RDPDriveMount.m — macOS placeholder mount for RDPDR-redirected client drives.
 *
 * After the MS-RDPEFS handshake, the server knows which client drives exist (by
 * name and device id). A full read/write mount would require macFUSE:
 *   brew install macfuse
 * and an in-process userspace filesystem that translates IRP ops (Create, Read,
 * Write, QueryDirectory, etc.) into WTS channel PDUs.  That is a large lift
 * tracked as a follow-up.
 *
 * This file implements Option C: create a visible folder on the Mac desktop so
 * the user can see which drives are redirected, and drop an INFO file there that
 * explains the current limitation.
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
             "Current state: handshake complete; drive is enumerated and a\n"
             "  FileStandardInformation query has been sent to read its size.\n"
             "\n"
             "Full read/write mount status: NOT YET IMPLEMENTED\n"
             "  A real mount requires macFUSE (https://macfuse.github.io) and an\n"
             "  in-process FUSE server that forwards IRP ops over the rdpdr channel.\n"
             "  Install macFUSE, then rebuild with -DRDPDR_FUSE_MOUNT=ON (future).\n"
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

        rdp_info("rdpdr: placeholder created at %s "
                 "(full FUSE mount requires macFUSE — see README.txt)",
                 driveDir.path.UTF8String);
    }
}
