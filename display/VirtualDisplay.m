#import "display/VirtualDisplay.h"
#import <CoreGraphics/CGVirtualDisplay.h>
#import <syslog.h>

/* CGVirtualDisplay is public API since macOS 12.4 (Monterey).
   The entitlement com.apple.developer.virtual-display is required
   for privileged multi-user scenarios; running as root does not
   need it in most cases. */

@interface VirtualDisplay ()
@property (nonatomic, strong) CGVirtualDisplay   *vd;
@property (nonatomic, assign) CGDirectDisplayID   did;
@property (nonatomic, assign) uint32_t w;
@property (nonatomic, assign) uint32_t h;
@property (nonatomic, assign) BOOL hiDPIEnabled;
@property (nonatomic, assign) BOOL created;
@end

@implementation VirtualDisplay

- (instancetype)initWithWidth:(uint32_t)width height:(uint32_t)height
                       hiDPI:(BOOL)hiDPI {
    if ((self = [super init])) {
        _w            = width;
        _h            = height;
        _hiDPIEnabled = hiDPI;
    }
    return self;
}

- (CGDirectDisplayID)displayID { return _did; }
- (uint32_t)width              { return _w; }
- (uint32_t)height             { return _h; }
- (BOOL)isCreated              { return _created; }

- (BOOL)create {
    if (_created) return YES;

    CGVirtualDisplayDescriptor *desc = [[CGVirtualDisplayDescriptor alloc] init];
    desc.name   = @"RDP Virtual Display";
    desc.width  = _w;
    desc.height = _h;
    desc.hiDPI  = _hiDPIEnabled;

    /* Physical size implying ~96 DPI at 1x, sensible default for RDP. */
    double dpi = _hiDPIEnabled ? 192.0 : 96.0;
    desc.sizeInMillimeters = CGSizeMake(
        (_w / dpi) * 25.4,
        (_h / dpi) * 25.4
    );
    desc.queue = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);

    _vd = [[CGVirtualDisplay alloc] initWithDescriptor:desc];
    if (!_vd) {
        syslog(LOG_ERR, "CGVirtualDisplay init failed");
        return NO;
    }

    _did     = _vd.displayID;
    _created = YES;
    syslog(LOG_INFO, "VirtualDisplay created: displayID=%u %ux%u",
           _did, _w, _h);
    return YES;
}

- (void)destroy {
    if (!_created) return;
    _vd      = nil;
    _did     = 0;
    _created = NO;
}

- (void)setResolutionWidth:(uint32_t)width height:(uint32_t)height {
    if (!_created) { _w = width; _h = height; return; }

    CGVirtualDisplaySettings *settings = [[CGVirtualDisplaySettings alloc] init];
    CGVirtualDisplayMode *mode = [[CGVirtualDisplayMode alloc]
        initWithWidth:width height:height refreshRate:60.0];
    settings.modes = @[mode];

    [_vd applySettings:settings completionQueue:
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0)
        completionHandler:^(BOOL ok, CGVirtualDisplay *disp) {
            (void)disp;
            if (ok) {
                self->_w = width;
                self->_h = height;
            }
        }];
}

@end
