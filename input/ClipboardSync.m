#import "input/ClipboardSync.h"
#import <AppKit/AppKit.h>
#define RDP_LOG_COMPONENT "clipboard"
#include "logging/RDPLog.h"

/*
 * NSPasteboardDidChangeNotification fires via NSDistributedNotificationCenter
 * whenever any process writes to the general pasteboard.
 * This replaces a 500ms poll with a zero-CPU push notification.
 * Available macOS 10.15+.
 */
static NSString *const kPasteboardChangedNote =
    @"com.apple.pasteboard.application-active";

@interface ClipboardSync ()
@property (nonatomic, assign) NSInteger lastChangeCount;
@end

@implementation ClipboardSync

- (void)start {
    _lastChangeCount = NSPasteboard.generalPasteboard.changeCount;

    [[NSDistributedNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(pasteboardChanged:)
               name:kPasteboardChangedNote
             object:nil
 suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];

    rdp_verbose("clipboard sync started (event-driven)");
}

- (void)stop {
    [[NSDistributedNotificationCenter defaultCenter] removeObserver:self];
    rdp_verbose("clipboard sync stopped");
}

- (void)pasteboardChanged:(NSNotification *)note {
    (void)note;
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    if (pb.changeCount == _lastChangeCount) return;
    _lastChangeCount = pb.changeCount;

    ClipboardSendBlock block = self.sendToClientBlock;
    if (!block) return;

    NSString *str = [pb stringForType:NSPasteboardTypeString];
    if (str) {
        NSData *utf16 = [str dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
        if (utf16) {
            rdp_verbose("clipboard: sending %zu UTF-16 bytes to client", utf16.length);
            block((const uint8_t *)utf16.bytes, utf16.length, RDP_CB_FORMAT_UNICODETEXT);
        }
        return;
    }
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (png) {
        rdp_verbose("clipboard: sending %zu PNG bytes to client", png.length);
        block((const uint8_t *)png.bytes, png.length, RDP_CB_FORMAT_PNG);
    }
}

- (void)receiveFromClient:(const uint8_t *)data length:(size_t)len format:(uint32_t)format {
    NSPasteboard *pb = NSPasteboard.generalPasteboard;
    [pb clearContents];
    if (format == RDP_CB_FORMAT_UNICODETEXT || format == RDP_CB_FORMAT_TEXT) {
        NSString *str = [[NSString alloc]
            initWithData:[NSData dataWithBytes:data length:len]
                encoding:NSUTF16LittleEndianStringEncoding];
        if (str) {
            [pb setString:str forType:NSPasteboardTypeString];
            _lastChangeCount = pb.changeCount;
            rdp_verbose("clipboard: received %zu bytes from client (text)", len);
        }
    } else if (format == RDP_CB_FORMAT_PNG) {
        [pb setData:[NSData dataWithBytes:data length:len] forType:NSPasteboardTypePNG];
        _lastChangeCount = pb.changeCount;
        rdp_verbose("clipboard: received %zu bytes from client (PNG)", len);
    }
}

@end
