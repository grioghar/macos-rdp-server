#import "daemon/DisplayControl.h"
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <dlfcn.h>
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
 * BLANKING STRATEGY — panel brightness, NOT display capture
 * ----------------------------------------------------------------------------
 * Privacy "blanking" here means dimming the BUILT-IN physical panel to black by
 * driving its backlight brightness to 0 via the private DisplayServices
 * framework. This is a pure side-channel to the panel hardware: it does NOT
 * change the display topology and does NOT trigger a WindowServer
 * reconfiguration, so the virtual/remote display keeps compositing normally and
 * the remote view is UNAFFECTED.
 *
 * This replaces the old CGDisplayCapture()+black-fill approach. Capturing a
 * display raised a privacy shield BUT the capture caused a WindowServer
 * reconfiguration that also stopped compositing to the virtual display, so the
 * remote went all-black. We must never do that again — there is no fallback to
 * CGDisplayCapture anywhere in this file.
 *
 * DisplayServices is a private framework. We resolve its two symbols at runtime
 * with dlopen/dlsym (no hard link dependency); if the framework or symbols are
 * unavailable on this hardware we log clearly and fall back to NO blanking —
 * the remote keeps working and we never crash.
 *
 *   extern int DisplayServicesSetBrightness(CGDirectDisplayID, float);
 *   extern int DisplayServicesGetBrightness(CGDirectDisplayID, float *);
 *
 * macOS single-session constraint: macOS has ONE GUI login session. This is
 * privacy DIMMING of the local panel, not a true session lock — we can't show
 * the login window locally while the remote keeps the unlocked desktop.
 *
 * Input monitoring uses a CGEventTap attached to the BACKGROUND run loop (the
 * one main.m already runs), so it works without the main thread. NSEvent global
 * monitors are avoided for the same run-loop reason.
 *
 * Everything here is best-effort: if dimming or the tap fails we log and keep
 * going. Remote video is the priority; we must never crash the session.
 */

/* DisplayServices private API signatures, resolved at runtime via dlsym. */
typedef int (*DSSetBrightnessFn)(CGDirectDisplayID, float);
typedef int (*DSGetBrightnessFn)(CGDirectDisplayID, float *);

@interface DisplayControl ()
@property (nonatomic, assign) CGDirectDisplayID virtualDisplayID;
@property (nonatomic, assign) BOOL started;
@property (nonatomic, assign) BOOL sharedMode;

/* Power management. */
@property (nonatomic, assign) IOPMAssertionID displaySleepAssertion;
@property (nonatomic, assign) IOPMAssertionID systemSleepAssertion;

/* DisplayServices brightness control, resolved lazily at runtime. */
@property (nonatomic, assign) void *displayServicesHandle;
@property (nonatomic, assign) DSSetBrightnessFn dsSetBrightness;
@property (nonatomic, assign) DSGetBrightnessFn dsGetBrightness;

/* Blanking state: maps each dimmed built-in display id -> its saved brightness
 * (to restore). Empty when nothing is dimmed. */
@property (nonatomic, assign) BOOL displaysDimmed;
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, NSNumber *> *savedBrightness;

/* Input monitor (CGEventTap on the background run loop). */
@property (nonatomic, assign) CFMachPortRef eventTap;
@property (nonatomic, assign) CFRunLoopSourceRef eventTapSource;
@property (nonatomic, assign) CFRunLoopRef eventTapRunLoop;

- (void)handleLocalUserActivity;
@end

/* CGEventTap callback: any physical interaction by the desk user un-dims. */
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
        _savedBrightness       = [NSMutableDictionary dictionary];
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

    /* Privacy DIMMING is the DEFAULT. We drive the built-in panel brightness to
     * 0 (a pure backlight change) so a bystander at the Mac can't watch the
     * remote session. This does NOT change the display topology, so the
     * virtual/remote display keeps compositing — the remote view is unaffected.
     * Set RDP_SHARED_MODE=1 to skip dimming entirely. If brightness control is
     * unavailable we fall back to NO dimming (never the old capture path). */
    [self dimBuiltInDisplays];
    [self installInputMonitor];
}

- (void)stop {
    if (!_started) return;
    _started = NO;

    [self removeInputMonitor];
    [self undimDisplays];
    [self releasePowerAssertions];
    [self closeDisplayServices];
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

#pragma mark - DisplayServices (private) brightness control

/* Resolve DisplayServicesSet/GetBrightness lazily via dlopen+dlsym. Returns YES
 * if both symbols are available. We deliberately do NOT hard-link the private
 * framework: if it's missing or renamed on some macOS build we simply skip
 * dimming instead of failing to launch. */
- (BOOL)ensureDisplayServices {
    if (_dsSetBrightness && _dsGetBrightness) return YES;

    if (!_displayServicesHandle) {
        _displayServicesHandle = dlopen(
            "/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices",
            RTLD_LAZY | RTLD_LOCAL);
        if (!_displayServicesHandle) {
            const char *e = dlerror();
            rdp_error("DisplayServices unavailable (dlopen: %s) — privacy dimming "
                      "DISABLED, remote unaffected", e ? e : "unknown");
            return NO;
        }
    }

    _dsSetBrightness = (DSSetBrightnessFn)dlsym(_displayServicesHandle,
                                                "DisplayServicesSetBrightness");
    _dsGetBrightness = (DSGetBrightnessFn)dlsym(_displayServicesHandle,
                                                "DisplayServicesGetBrightness");
    if (!_dsSetBrightness || !_dsGetBrightness) {
        rdp_error("DisplayServices brightness symbols missing (set=%p get=%p) — "
                  "privacy dimming DISABLED, remote unaffected",
                  (void *)_dsSetBrightness, (void *)_dsGetBrightness);
        _dsSetBrightness = NULL;
        _dsGetBrightness = NULL;
        return NO;
    }
    return YES;
}

- (void)closeDisplayServices {
    _dsSetBrightness = NULL;
    _dsGetBrightness = NULL;
    if (_displayServicesHandle) {
        dlclose(_displayServicesHandle);
        _displayServicesHandle = NULL;
    }
}

#pragma mark - Privacy dimming of the built-in display

/* Enumerate active displays and dim every BUILT-IN one that is not the virtual
 * display to brightness 0. This is a backlight change only — no display-config
 * change — so the virtual/remote display keeps compositing normally. The
 * original brightness of each affected display is saved for restore. */
- (void)dimBuiltInDisplays {
    if (![self ensureDisplayServices]) {
        rdp_info("privacy dimming skipped — DisplayServices brightness control "
                 "unavailable on this hardware; remote view works normally");
        return;
    }

    CGDirectDisplayID active[16];
    uint32_t count = 0;
    CGError ce = CGGetActiveDisplayList(16, active, &count);
    if (ce != kCGErrorSuccess) {
        rdp_error("CGGetActiveDisplayList failed (%d) — cannot dim, continuing", ce);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        CGDirectDisplayID did = active[i];
        if (did == _virtualDisplayID) {
            rdp_verbose("display %u is the virtual/remote display — left lit", did);
            continue;
        }
        /* Only dim the physical built-in panel(s); leave any external monitors
         * alone, and never touch the display the remote captures. */
        if (!CGDisplayIsBuiltin(did)) {
            rdp_verbose("display %u is not built-in — left lit", did);
            continue;
        }

        /* Save the current brightness so we can restore it later. */
        float original = 1.0f;
        int gr = _dsGetBrightness(did, &original);
        if (gr != 0) {
            rdp_error("DisplayServicesGetBrightness(%u) failed (%d) — "
                      "display NOT dimmed (can't safely restore)", did, gr);
            continue;
        }

        int sr = _dsSetBrightness(did, 0.0f);
        if (sr != 0) {
            rdp_error("DisplayServicesSetBrightness(%u, 0) failed (%d) — "
                      "display NOT dimmed", did, sr);
            continue;
        }

        _savedBrightness[@(did)] = @(original);
        rdp_info("privacy-dimmed built-in display %u (brightness %.2f -> 0)",
                 did, original);
    }

    if (_savedBrightness.count == 0) {
        rdp_info("no built-in display to dim (only the virtual/remote display "
                 "is active, or none accepted brightness control) — nothing to do");
        return;
    }
    _displaysDimmed = YES;
}

/* Restore every dimmed display to its saved brightness. */
- (void)undimDisplays {
    if (!_displaysDimmed) return;

    for (NSNumber *key in _savedBrightness) {
        CGDirectDisplayID did = (CGDirectDisplayID)key.unsignedIntValue;
        float original = _savedBrightness[key].floatValue;
        if (_dsSetBrightness) {
            int sr = _dsSetBrightness(did, original);
            if (sr == 0)
                rdp_info("restored brightness %.2f on display %u", original, did);
            else
                rdp_error("DisplayServicesSetBrightness(%u, %.2f) restore failed (%d)",
                          did, original, sr);
        }
    }
    [_savedBrightness removeAllObjects];
    _displaysDimmed = NO;
}

#pragma mark - Return control to the local user

/* Re-dim policy: once the desk user physically interacts we restore brightness
 * and DO NOT re-dim for the rest of this session. Rationale: re-dimming under
 * the user's hands would fight an actively-present person at the keyboard, which
 * is worse than the small privacy window of a present, deliberate user. Privacy
 * is fully restored on the next session connect. */
- (void)installInputMonitor {
    if (!_displaysDimmed) return;   /* nothing to give back */

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
        rdp_error("CGEventTapCreate failed — desk user cannot auto-restore brightness "
                  "(needs Accessibility/Input Monitoring). Dim stays until disconnect.");
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
    if (!_displaysDimmed) return;   /* already handed back */
    rdp_info("local user activity detected — restoring the screen to the desk user");
    [self undimDisplays];
    /* Tear down the tap; we won't re-dim this session. */
    [self removeInputMonitor];
}

@end
