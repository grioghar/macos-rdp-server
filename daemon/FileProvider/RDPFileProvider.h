/* RDPFileProvider.h — NSFileProviderManager wrapper for RDPDR drive mounting.
 *
 * Registers an NSFileProviderDomain for each Windows drive that the RDP client
 * advertises via MS-RDPEFS DEVICE_LIST_ANNOUNCE. The actual I/O (IRP forwarding
 * over the WTS rdpdr channel) is implemented in a later phase; this skeleton
 * only registers/removes domains and logs the attempt.
 *
 * Availability: macOS 12+ (NSFileProviderReplicatedExtension API).
 *               The entire class is compiled away on older SDKs via the
 *               __has_include guard in the .m file.
 *
 * Thread-safety: all methods dispatch to the main queue internally.
 */

#pragma once

#if __has_include(<FileProvider/FileProvider.h>)

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface RDPFileProvider : NSObject

/* Register a File Provider domain for the named drive.
 * driveName  — the MS-DOS device name (e.g. "C", "D", "Desktop").
 * deviceId   — the RDPDR device id from DEVICE_LIST_ANNOUNCE.
 *
 * The domain identifier is "rdp.<driveName>" so it is stable across
 * reconnects for the same drive letter.  Idempotent: calling again with the
 * same name is a no-op (NSFileProviderManager ignores duplicate additions). */
+ (void)mountDrive:(NSString *)driveName deviceId:(uint32_t)deviceId;

/* Remove the File Provider domain for the named drive.
 * Safe to call when the domain was never added. */
+ (void)unmountDrive:(NSString *)driveName;

@end

NS_ASSUME_NONNULL_END

#endif /* __has_include(<FileProvider/FileProvider.h>) */
