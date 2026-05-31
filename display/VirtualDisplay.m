#import "display/VirtualDisplay.h"
#import <CoreGraphics/CoreGraphics.h>
#import <unistd.h>
#define RDP_LOG_COMPONENT "display"
#include "logging/RDPLog.h"

@interface VirtualDisplay ()
@property (nonatomic, assign) CGDirectDisplayID did;
@property (nonatomic, assign) uint32_t w;
@property (nonatomic, assign) uint32_t h;
@property (nonatomic, assign) BOOL created;

#if MACOS_RDP_VIRTUAL_DISPLAY
/* Opaque pointer so the compiler doesn't need CGVirtualDisplay headers here. */
@property (nonatomic, strong) id vdObject;
#endif
@end

@implementation VirtualDisplay

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height {
    if ((self = [super init])) {
        _w = width;
        _h = height;
    }
    return self;
}

- (CGDirectDisplayID)displayID { return _did; }
- (uint32_t)width              { return _w; }
- (uint32_t)height             { return _h; }

- (BOOL)create {
    if (_created) return YES;

#if MACOS_RDP_VIRTUAL_DISPLAY
    [self createVirtualDisplay];
#else
    /* Default: capture the main display. Physical display is undisturbed
       because RDP sessions run in a separate window-server context when
       the daemon is launched as a login item for a specific user. */
    _did = CGMainDisplayID();
    rdp_info("using main display %u (%ux%u) — build with MACOS_RDP_VIRTUAL_DISPLAY=1 "
             "for a dedicated virtual display (requires Apple entitlement)", _did, _w, _h);
#endif

    _created = YES;
    return YES;
}

- (void)destroy {
    if (!_created) return;
#if MACOS_RDP_VIRTUAL_DISPLAY
    _vdObject = nil;
#endif
    _did = 0;
    _created = NO;
    rdp_verbose("display released");
}

- (void)setResolutionWidth:(uint32_t)width height:(uint32_t)height {
    _w = width;
    _h = height;
    rdp_verbose("resolution set to %ux%u (takes effect on next session)", width, height);
}

#if MACOS_RDP_VIRTUAL_DISPLAY
- (void)createVirtualDisplay {
    /*
     * Create a CGVirtualDisplay sized exactly to the RDP client (e.g. 3440x1440)
     * so there is no pillarbox/scaling and pointer mapping is 1:1. CGVirtualDisplay
     * is a PRIVATE CoreGraphics API (CGVirtualDisplayDescriptor / *Mode / *Settings),
     * driven here entirely via NSClassFromString + KVC + NSInvocation so a missing
     * class / wrong ABI / thrown exception cleanly falls back to the main display
     * (pillarboxed) instead of crashing the daemon.
     */
    _did = CGMainDisplayID();   /* safe default */
    @try {
        Class descClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
        Class dispClass = NSClassFromString(@"CGVirtualDisplay");
        Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
        Class setClass  = NSClassFromString(@"CGVirtualDisplaySettings");
        if (!descClass || !dispClass || !modeClass || !setClass) {
            rdp_error("CGVirtualDisplay classes unavailable on this macOS — "
                      "using main display (pillarboxed)");
            return;
        }

        id desc = [[descClass alloc] init];
        [desc setValue:@"RDP Virtual Display" forKey:@"name"];
        [desc setValue:@(_w) forKey:@"maxPixelsWide"];
        [desc setValue:@(_h) forKey:@"maxPixelsHigh"];
        double dpi = 100.0;
        CGSize mm = CGSizeMake((_w / dpi) * 25.4, (_h / dpi) * 25.4);
        [desc setValue:[NSValue valueWithBytes:&mm objCType:@encode(CGSize)]
                forKey:@"sizeInMillimeters"];
        @try { [desc setValue:@(0x5244500u) forKey:@"productID"]; } @catch (id e) {}
        @try { [desc setValue:@(0x5244500u) forKey:@"vendorID"];  } @catch (id e) {}
        @try { [desc setValue:@(1)          forKey:@"serialNum"]; } @catch (id e) {}
        @try { [desc setValue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0)
                       forKey:@"queue"]; } @catch (id e) {}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        id vd = [[dispClass alloc] performSelector:@selector(initWithDescriptor:)
                                         withObject:desc];
#pragma clang diagnostic pop
        if (!vd) { rdp_error("CGVirtualDisplay init failed — main display"); return; }

        /* Build a mode via NSInvocation (scalar args). Signature is assumed
         * (unsigned int width, unsigned int height, double refreshRate). */
        id mode = nil;
        SEL modeSel = @selector(initWithWidth:height:refreshRate:);
        if ([modeClass instancesRespondToSelector:modeSel]) {
            id m = [modeClass alloc];
            NSMethodSignature *sig = [m methodSignatureForSelector:modeSel];
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
            inv.target = m; inv.selector = modeSel;
            unsigned int w = _w, h = _h; double rr = 60.0;
            [inv setArgument:&w atIndex:2];
            [inv setArgument:&h atIndex:3];
            [inv setArgument:&rr atIndex:4];
            [inv invoke];
            void *ret = NULL; [inv getReturnValue:&ret];
            mode = (__bridge id)ret;
        }
        if (!mode) { rdp_error("CGVirtualDisplayMode init failed — main display"); return; }

        id settings = [[setClass alloc] init];
        [settings setValue:@[mode] forKey:@"modes"];
        @try { [settings setValue:@(0) forKey:@"hiDPI"]; } @catch (id e) {}

        BOOL applied = NO;
        SEL applySel = @selector(applySettings:);
        if ([vd respondsToSelector:applySel]) {
            NSMethodSignature *sig2 = [vd methodSignatureForSelector:applySel];
            NSInvocation *inv2 = [NSInvocation invocationWithMethodSignature:sig2];
            inv2.target = vd; inv2.selector = applySel;
            [inv2 setArgument:&settings atIndex:2];
            [inv2 invoke];
            [inv2 getReturnValue:&applied];
        }

        CGDirectDisplayID vdid = 0;
        @try { vdid = (CGDirectDisplayID)[[vd valueForKey:@"displayID"] unsignedIntValue]; }
        @catch (id e) {}
        if (vdid != 0) {
            _vdObject = vd;
            _did = vdid;
            rdp_info("virtual display created: displayID=%u %ux%u (applied=%d)",
                     _did, _w, _h, applied);

            /* Make the virtual display the MAIN display so the menu bar, Dock, and
             * newly-opened windows land ON it. Otherwise apps open on the built-in
             * screen, invisible to the RDP session ("windows never pop up"). Give the
             * new display a moment to register, then put it at the global origin
             * (0,0) = main, and move the former main (built-in) to its right. */
            usleep(400000);
            CGDirectDisplayID oldMain = CGMainDisplayID();
            if (oldMain != _did) {
                CGDisplayConfigRef dcfg = NULL;
                if (CGBeginDisplayConfiguration(&dcfg) == kCGErrorSuccess) {
                    CGConfigureDisplayOrigin(dcfg, _did, 0, 0);
                    CGConfigureDisplayOrigin(dcfg, oldMain, (int32_t)_w, 0);
                    CGError ce = CGCompleteDisplayConfiguration(dcfg, kCGConfigureForSession);
                    rdp_info("virtual display %u set as main (built-in %u moved aside, rc=%d)",
                             _did, oldMain, ce);
                }
            }
        } else {
            rdp_error("virtual display has no displayID — main display");
        }
    } @catch (NSException *e) {
        rdp_error("virtual display creation threw (%s) — main display",
                  e.reason.UTF8String ?: "?");
        _did = CGMainDisplayID();
    }
}
#endif

@end
