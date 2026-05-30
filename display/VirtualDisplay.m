#import "display/VirtualDisplay.h"
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
     * CGVirtualDisplay requires com.apple.developer.virtual-display entitlement.
     * Compile with -DMACOS_RDP_VIRTUAL_DISPLAY=1 and sign with that entitlement.
     *
     * Uses NSClassFromString to avoid a hard link against the symbols so the
     * binary still launches without the entitlement (it just falls back above).
     */
    Class descClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class dispClass = NSClassFromString(@"CGVirtualDisplay");
    if (!descClass || !dispClass) {
        rdp_verbose("CGVirtualDisplay not available (entitlement missing?), "
                    "falling back to main display");
        _did = CGMainDisplayID();
        return;
    }

    id desc = [[descClass alloc] init];
    [desc setValue:@"RDP Virtual Display"    forKey:@"name"];
    [desc setValue:@(_w)                     forKey:@"width"];
    [desc setValue:@(_h)                     forKey:@"height"];
    [desc setValue:@NO                       forKey:@"hiDPI"];
    double dpi = 96.0;
    CGSize mm = CGSizeMake((_w / dpi) * 25.4, (_h / dpi) * 25.4);
    [desc setValue:[NSValue valueWithBytes:&mm objCType:@encode(CGSize)]
            forKey:@"sizeInMillimeters"];
    [desc setValue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0)
            forKey:@"queue"];

    id vd = [[dispClass alloc] initWithDescriptor:desc];
    if (!vd) {
        rdp_error("CGVirtualDisplay alloc failed, using main display");
        _did = CGMainDisplayID();
        return;
    }
    _vdObject = vd;
    _did = (CGDirectDisplayID)[[vd valueForKey:@"displayID"] unsignedIntValue];
    rdp_info("virtual display created: displayID=%u %ux%u", _did, _w, _h);
}
#endif

@end
