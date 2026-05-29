#import "input/ClipboardSync.h"
#import <AppKit/AppKit.h>

@interface ClipboardSync ()
@property (nonatomic, strong) NSTimer   *pollTimer;
@property (nonatomic, assign) NSInteger  lastChangeCount;
@end

@implementation ClipboardSync

- (void)start {
    _lastChangeCount = [NSPasteboard generalPasteboard].changeCount;
    /* Poll the pasteboard every 500ms — NSPasteboard doesn't offer push
       notifications for changes made by other processes. */
    _pollTimer = [NSTimer scheduledTimerWithTimeInterval:0.5
                                                 target:self
                                               selector:@selector(checkPasteboard)
                                               userInfo:nil
                                                repeats:YES];
}

- (void)stop {
    [_pollTimer invalidate];
    _pollTimer = nil;
}

- (void)checkPasteboard {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if (pb.changeCount == _lastChangeCount) return;
    _lastChangeCount = pb.changeCount;

    ClipboardSendBlock block = self.sendToClientBlock;
    if (!block) return;

    /* Prefer plain Unicode text. */
    NSString *str = [pb stringForType:NSPasteboardTypeString];
    if (str) {
        /* Encode as UTF-16LE with BOM (CF_UNICODETEXT). */
        NSData *utf16 = [str dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
        if (utf16) {
            block((const uint8_t *)utf16.bytes, utf16.length,
                  RDP_CB_FORMAT_UNICODETEXT);
        }
        return;
    }

    /* Fall back to PNG image if available. */
    NSData *png = [pb dataForType:NSPasteboardTypePNG];
    if (png) {
        block((const uint8_t *)png.bytes, png.length, RDP_CB_FORMAT_PNG);
    }
}

- (void)receiveFromClient:(const uint8_t *)data length:(size_t)len
                   format:(uint32_t)format {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];

    if (format == RDP_CB_FORMAT_UNICODETEXT || format == RDP_CB_FORMAT_TEXT) {
        NSData *raw = [NSData dataWithBytes:data length:len];
        NSString *str = [[NSString alloc]
            initWithData:raw encoding:NSUTF16LittleEndianStringEncoding];
        if (str) {
            [pb setString:str forType:NSPasteboardTypeString];
            _lastChangeCount = pb.changeCount;
        }
    } else if (format == RDP_CB_FORMAT_PNG) {
        NSData *imgData = [NSData dataWithBytes:data length:len];
        [pb setData:imgData forType:NSPasteboardTypePNG];
        _lastChangeCount = pb.changeCount;
    }
}

@end
