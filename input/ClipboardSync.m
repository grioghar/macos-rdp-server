#import "input/ClipboardSync.h"
#import <AppKit/AppKit.h>
#define RDP_LOG_COMPONENT "clipboard"
#include "logging/RDPLog.h"

/*
 * macOS provides NO pasteboard-change notification (NSPasteboard has no KVO and
 * no NSNotification for content changes — the previously-used
 * "com.apple.pasteboard.application-active" distributed note does not fire on
 * copy). The only reliable way to detect a copy is to poll NSPasteboard.changeCount.
 * We poll a few times a second; when idle this is just an integer comparison.
 */

@interface ClipboardSync ()
@property (nonatomic, assign) NSInteger lastChangeCount;
@property (nonatomic, strong) dispatch_source_t pollTimer;
@end

@implementation ClipboardSync

- (void)start {
    _lastChangeCount = NSPasteboard.generalPasteboard.changeCount;

    _pollTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
                    dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    dispatch_source_set_timer(_pollTimer, dispatch_time(DISPATCH_TIME_NOW, 0),
                              (uint64_t)(NSEC_PER_SEC / 4), NSEC_PER_SEC / 8); /* ~4 Hz */
    __weak typeof(self) weak = self;
    dispatch_source_set_event_handler(_pollTimer, ^{ [weak pollPasteboard]; });
    dispatch_resume(_pollTimer);

    rdp_verbose("clipboard sync started (polling changeCount ~4Hz)");
}

- (void)stop {
    if (_pollTimer) { dispatch_source_cancel(_pollTimer); _pollTimer = nil; }
    rdp_verbose("clipboard sync stopped");
}

/* Mac -> Windows: when the pasteboard changes, advertise the new contents. */
- (void)pollPasteboard {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    NSInteger cc = pb.changeCount;
    if (cc == _lastChangeCount) return;
    _lastChangeCount = cc;

    ClipboardSendBlock block = self.sendToClientBlock;
    if (!block) return;

    NSString *str = [pb stringForType:NSPasteboardTypeString];
    if (str) {
        NSData *utf16 = [str dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
        if (utf16) {
            /* MS-RDPECLIP: CF_UNICODETEXT data is expected to be NUL-terminated.
             * NSString's encoding does not append one, so add a trailing U+0000.
             * Unicode/emoji are preserved (surrogate pairs are just more code
             * units); large blocks are sent in full (no truncation). */
            NSMutableData *out = [utf16 mutableCopy];
            const uint16_t nul = 0;
            [out appendBytes:&nul length:sizeof(nul)];
            rdp_verbose("clipboard: Mac copy -> %zu UTF-16 bytes to client",
                        (size_t)out.length);
            block((const uint8_t *)out.bytes, out.length, RDP_CB_FORMAT_UNICODETEXT);
        }
        return;
    }
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (png) {
        rdp_verbose("clipboard: Mac copy -> %zu PNG bytes to client", (size_t)png.length);
        block((const uint8_t *)png.bytes, png.length, RDP_CB_FORMAT_PNG);
    }
}

/* Windows -> Mac: client sent clipboard data; place it on the Mac pasteboard. */
- (void)receiveFromClient:(const uint8_t *)data length:(size_t)len format:(uint32_t)format {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    if (format == RDP_CB_FORMAT_UNICODETEXT || format == RDP_CB_FORMAT_TEXT) {
        NSString *str = nil;
        if (format == RDP_CB_FORMAT_TEXT) {
            /* CF_TEXT is ANSI/codepage bytes, NUL-terminated. */
            str = [[NSString alloc] initWithData:[NSData dataWithBytes:data length:len]
                                        encoding:NSWindowsCP1252StringEncoding];
        } else {
            /* CF_UNICODETEXT is UTF-16LE and mstsc terminates it with a U+0000.
             * Decoding the trailing NUL yields a stray \0 the Mac pasteboard keeps
             * verbatim, so trim a single trailing UTF-16 NUL (2 bytes) if present.
             * Full length is honoured otherwise — no truncation of large blocks. */
            size_t blen = len;
            if (blen >= 2 && data[blen - 1] == 0 && data[blen - 2] == 0)
                blen -= 2;
            str = [[NSString alloc] initWithData:[NSData dataWithBytes:data length:blen]
                                        encoding:NSUTF16LittleEndianStringEncoding];
        }
        if (str) {
            [pb setString:str forType:NSPasteboardTypeString];
            _lastChangeCount = pb.changeCount;  /* don't bounce it back to the client */
            rdp_verbose("clipboard: received %zu bytes from client (text)", len);
        } else {
            rdp_verbose("clipboard: failed to decode %zu text bytes from client", len);
        }
    } else if (format == RDP_CB_FORMAT_PNG) {
        [pb setData:[NSData dataWithBytes:data length:len] forType:NSPasteboardTypePNG];
        _lastChangeCount = pb.changeCount;
        rdp_verbose("clipboard: received %zu bytes from client (PNG)", len);
    }
}

@end
