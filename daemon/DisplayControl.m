#import "daemon/DisplayControl.h"
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <unistd.h>
#define RDP_LOG_COMPONENT "displayctl"
#include "logging/RDPLog.h"

/*
 * RUN-LOOP / THREADING NOTE (important — read before changing blanking strategy)
 * ----------------------------------------------------------------------------
 * The daemon's MAIN thread blocks in kevent() for its whole life (see
 * daemon/main.m); the only running run loop is a CFRunLoop spun on a background
 * dispatch global queue. There is therefore NO running main run loop and no
 * NSApplication event loop.
 *
 * Consequences for blanking:
 *   - A black NSWindow / NSScreenSaverWindowLevel overlay needs AppKit on the
 *     main thread AND a draining main run loop to actually composite and stay on
 *     top. Neither exists here, so an NSWindow overlay is unreliable headless.
 *   - CGDisplayCapture() + a fill of the captured display's drawing context is
 *     a pure CoreGraphics path: it does NOT need AppKit or any run loop. The
 *     capture itself raises the display above all normal windows (the shield),
 *     so a black fill is a true privacy blank. This is the strategy we use.
 *
 * Input monitoring uses a CGEventTap attached to the BACKGROUND run loop (the
 * one main.m already runs), so it works without the main thread. NSEvent global
 * monitors are avoided for the same run-loop reason.
 *
 * Everything here is best-effort: if blanking or the tap fails we log and keep
 * going. Remote video is the priority; we must never crash the session.
 */

@interface DisplayControl ()
@property (nonatomic, assign) CGDirectDisplayID virtualDisplayID;
@property (nonatomic, assign) BOOL started;
@property (nonatomic, assign) BOOL sharedMode;

/* Power management. */
@property (nonatomic, assign) IOPMAssertionID displaySleepAssertion;
@property (nonatomic, assign) IOPMAssertionID systemSleepAssertion;

/* Blanking state. */
@property (nonatomic, assign) BOOL displaysCaptured;
@property (nonatomic, strong) NSArray<NSNumber *> *blankedDisplays;

/* Input monitor (CGEventTap on the background run loop). */
@property (nonatomic, assign) CFMachPortRef eventTap;
@property (nonatomic, assign) CFRunLoopSourceRef eventTapSource;
@property (nonatomic, assign) CFRunLoopRef eventTapRunLoop;

- (void)handleLocalUserActivity;
@end

/* CGEventTap callback: any physical interaction by the desk user un-blanks. */
static CGEventRef rdp_event_tap_cb(CGEventTapProxy proxy, CGEventType type,
                                   CGEventRef event, void *ud) {
    (void)proxy;
    DisplayControl *self = (__bridge DisplayControl *)ud;
    if (type == kCGEventTapDisabledByTimeout ||
        type == kCGEventTapDisabledByUserInput) {
        /* The system can disable a tap that is too slow; re-enable it. */
        if (self.eventTap) CGEventTapEnable(self.eventTap, true);
        return event;
    }
    [self handleLocalUserActivity];
    return event;   /* never swallow the user's real input */
}

@implementation DisplayControl

- (instancetype)initWithVirtualDisplayID:(CGDirectDisplayID)virtualDisplayID {
    if ((self = [super init])) {
        _virtualDisplayID      = virtualDisplayID;
        _displaySleepAssertion = kIOPMNullAssertionID;
        _systemSleepAssertion  = kIOPMNullAssertionID;
        const char *shared     = getenv("RDP_SHARED_MODE");
        _sharedMode            = (shared && strcmp(shared, "1") == 0);
    }
    return self;
}

- (void)start {
    if (_started) return;
    _started = YES;

    [self takePowerAssertions];

    if (_sharedMode) {
        rdp_info("RDP_SHARED_MODE=1 — privacy blanking SKIPPED (desk user and "
                 "remote user share the screen)");
        return;
    }

    /* CGDisplayCapture + drawing-context fill is thread-safe and needs no run
     * loop, so we run it inline on the session queue — the blank takes effect
     * immediately and we avoid dispatching onto the blocked main thread. */
    [self blankBuiltInDisplays];
    [self installInputMonitor];
}

- (void)stop {
    if (!_started) return;
    _started = NO;

    [self removeInputMonitor];
    [self unblankDisplays];
    [self releasePowerAssertions];
    rdp_verbose("display control stopped");
}

#pragma mark - Power management (replaces `caffeinate -d`)

- (void)takePowerAssertions {
    /* Wake the display NOW if it was asleep when the client connected. */
    IOPMAssertionID activityID = kIOPMNullAssertionID;
    IOReturn ar = IOPMAssertionDeclareUserActivity(CFSTR("rdp-connect"),
                                                   kIOPMUserActiveLocal,
                                                   &activityID);
    if (ar == kIOReturnSuccess)
        rdp_info("declared user activity — display woken for incoming session");
    else
        rdp_error("IOPMAssertionDeclareUserActivity failed (0x%x)", ar);

    /* Hold the display awake for the whole session. */
    IOReturn dr = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleDisplaySleep,
        kIOPMAssertionLevelOn,
        CFSTR("macos-rdp: remote session active (display)"),
        &_displaySleepAssertion);
    if (dr == kIOReturnSuccess)
        rdp_info("holding PreventUserIdleDisplaySleep for session");
    else {
        _displaySleepAssertion = kIOPMNullAssertionID;
        rdp_error("PreventUserIdleDisplaySleep assertion failed (0x%x)", dr);
    }

    /* Also keep the system from idle-sleeping so capture/encode keep running. */
    IOReturn sr = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep,
        kIOPMAssertionLevelOn,
        CFSTR("macos-rdp: remote session active (system)"),
        &_systemSleepAssertion);
    if (sr == kIOReturnSuccess)
        rdp_verbose("holding PreventUserIdleSystemSleep for session");
    else {
        _systemSleepAssertion = kIOPMNullAssertionID;
        rdp_error("PreventUserIdleSystemSleep assertion failed (0x%x)", sr);
    }
}

- (void)releasePowerAssertions {
    if (_displaySleepAssertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(_displaySleepAssertion);
        _displaySleepAssertion = kIOPMNullAssertionID;
        rdp_verbose("released display-sleep assertion");
    }
    if (_systemSleepAssertion != kIOPMNullAssertionID) {
        IOPMAssertionRelease(_systemSleepAssertion);
        _systemSleepAssertion = kIOPMNullAssertionID;
        rdp_verbose("released system-sleep assertion");
    }
}

#pragma mark - Privacy blanking of the built-in display

/* Enumerate active displays, capture every BUILT-IN one that is not the virtual
 * display, and fill it black. CGDisplayCapture raises the display above normal
 * windows and gives us an exclusive drawing context — a true privacy shield
 * that needs no AppKit run loop. */
- (void)blankBuiltInDisplays {
    CGDirectDisplayID active[16];
    uint32_t count = 0;
    CGError ce = CGGetActiveDisplayList(16, active, &count);
    if (ce != kCGErrorSuccess) {
        rdp_error("CGGetActiveDisplayList failed (%d) — cannot blank, continuing", ce);
        return;
    }

    NSMutableArray<NSNumber *> *blanked = [NSMutableArray array];
    for (uint32_t i = 0; i < count; i++) {
        CGDirectDisplayID did = active[i];
        if (did == _virtualDisplayID) {
            rdp_verbose("display %u is the virtual/remote display — left visible", did);
            continue;
        }
        /* Only blank the physical built-in panel(s); leave any extra external
         * monitors alone (they may be the desk user's, but more importantly we
         * must never accidentally blank the one the remote captures). */
        if (!CGDisplayIsBuiltin(did)) {
            rdp_verbose("display %u is not built-in — left visible", did);
            continue;
        }

        CGError cap = CGDisplayCapture(did);
        if (cap != kCGErrorSuccess) {
            rdp_error("CGDisplayCapture(%u) failed (%d) — display NOT blanked", did, cap);
            continue;
        }
        [self fillCapturedDisplayBlack:did];
        [blanked addObject:@(did)];
        rdp_info("privacy-blanked built-in display %u", did);
    }

    if (blanked.count == 0) {
        rdp_info("no built-in display to blank (only the virtual/remote display "
                 "is active) — nothing to do");
        return;
    }
    _blankedDisplays  = blanked;
    _displaysCaptured = YES;
}

- (void)fillCapturedDisplayBlack:(CGDirectDisplayID)did {
    /* The captured display gives us an exclusive CG drawing context we can fill
     * with opaque black. Available only while the display is captured. */
    CGContextRef ctx = CGDisplayGetDrawingContext(did);
    if (!ctx) {
        rdp_error("no drawing context for captured display %u — leaving as-is", did);
        return;
    }
    size_t w = CGDisplayPixelsWide(did);
    size_t h = CGDisplayPixelsHigh(did);
    CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h));
    CGContextFlush(ctx);
}

- (void)unblankDisplays {
    if (!_displaysCaptured) return;
    for (NSNumber *n in _blankedDisplays) {
        CGDirectDisplayID did = (CGDirectDisplayID)n.unsignedIntValue;
        CGError rc = CGDisplayRelease(did);
        if (rc == kCGErrorSuccess)
            rdp_info("released privacy blank on display %u", did);
        else
            rdp_error("CGDisplayRelease(%u) failed (%d)", did, rc);
    }
    /* Belt-and-suspenders: release any displays still captured by this process. */
    CGReleaseAllDisplays();
    _blankedDisplays  = nil;
    _displaysCaptured = NO;
}

#pragma mark - Return control to the local user

/* Re-blank policy: once the desk user physically interacts we hand the screen
 * back and DO NOT re-blank for the rest of this session. Rationale: re-blanking
 * under the user's hands would fight an actively-present person at the keyboard,
 * which is worse than the small privacy window of a present, deliberate user.
 * Privacy is fully restored on the next session connect. */
- (void)installInputMonitor {
    if (!_displaysCaptured) return;   /* nothing to give back */

    CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved)   |
                       CGEventMaskBit(kCGEventLeftMouseDown) |
                       CGEventMaskBit(kCGEventRightMouseDown)|
                       CGEventMaskBit(kCGEventKeyDown)       |
                       CGEventMaskBit(kCGEventScrollWheel);

    /* Listen-only tap (we never modify/drop events). Attach to the background
     * run loop that main.m runs; the main thread is blocked in kevent(). */
    CFMachPortRef tap = CGEventTapCreate(kCGHIDEventTap, kCGTailAppendEventTap,
                                         kCGEventTapOptionListenOnly, mask,
                                         rdp_event_tap_cb, (__bridge void *)self);
    if (!tap) {
        rdp_error("CGEventTapCreate failed — desk user cannot auto-reclaim screen "
                  "(needs Accessibility/Input Monitoring). Blank stays until disconnect.");
        return;
    }
    CFRunLoopSourceRef src = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap, 0);
    /* The main run loop is not draining (main thread blocks in kevent), so add
     * the source on a background global-queue thread — main.m runs a CFRunLoop
     * there, so that run loop is actually spinning and will service the tap. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        CFRunLoopRef cur = CFRunLoopGetCurrent();
        CFRunLoopAddSource(cur, src, kCFRunLoopCommonModes);
        CGEventTapEnable(tap, true);
        self.eventTapRunLoop = cur;
        rdp_verbose("input monitor armed — desk user can reclaim the screen");
    });
    _eventTap       = tap;
    _eventTapSource = src;
}

- (void)removeInputMonitor {
    if (_eventTap) {
        CGEventTapEnable(_eventTap, false);
    }
    if (_eventTapSource) {
        if (_eventTapRunLoop)
            CFRunLoopRemoveSource(_eventTapRunLoop, _eventTapSource, kCFRunLoopCommonModes);
        CFRelease(_eventTapSource);
        _eventTapSource = NULL;
    }
    if (_eventTap) {
        CFRelease(_eventTap);
        _eventTap = NULL;
    }
    _eventTapRunLoop = NULL;
}

- (void)handleLocalUserActivity {
    if (!_displaysCaptured) return;   /* already handed back */
    rdp_info("local user activity detected — returning the screen to the desk user");
    [self unblankDisplays];
    /* Tear down the tap; we won't re-blank this session. */
    [self removeInputMonitor];
}

@end
