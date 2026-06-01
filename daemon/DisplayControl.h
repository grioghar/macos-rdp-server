#pragma once
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

NS_ASSUME_NONNULL_BEGIN

/*
 * DisplayControl: per-session control of the LOCAL Mac display, replacing the
 * manual `caffeinate -d` crutch and adding privacy for the desk user.
 *
 * Responsibilities, all scoped to one RDP session lifetime:
 *
 *  1. Wake-on-connect + keep awake — declares user activity (wakes a sleeping
 *     display) and holds IOPMAssertion(s) so the display/system never idle-sleep
 *     while the session is live. Released on stop.
 *
 *  2. Privacy dimming — drives the BUILT-IN physical panel's backlight
 *     brightness to 0 (via the private DisplayServices framework, resolved at
 *     runtime with dlopen/dlsym) so a bystander at the Mac can't watch the
 *     remote session. This is a pure backlight change — it does NOT touch the
 *     display topology — so the VIRTUAL display the remote captures keeps
 *     compositing and the remote view is UNAFFECTED. The virtual display is
 *     never dimmed. Default-on; skipped entirely in shared mode, and skipped
 *     (with the remote still working) if brightness control is unavailable.
 *
 *  3. Return control to the desk user — a global input monitor restores the
 *     saved brightness the moment the desk user physically touches the Mac
 *     (mouse move / key / click). It stays restored for the rest of the
 *     session (see DisplayControl.m for rationale).
 *
 * macOS single-session constraint: macOS has ONE GUI login session, so we can't
 * show the login window locally while the remote keeps the unlocked desktop on
 * the same session. "Lock the local screen" is therefore implemented as privacy
 * DIMMING of the built-in panel only — not a real screen lock.
 *
 * Shared mode (env RDP_SHARED_MODE=="1"): skip dimming so the desk user and
 * remote user both see and control the desktop simultaneously.
 */
@interface DisplayControl : NSObject

/* virtualDisplayID is the display the remote session captures and MUST NOT be
 * dimmed. Every other active display that is built-in is dimmed for privacy. */
- (instancetype)initWithVirtualDisplayID:(CGDirectDisplayID)virtualDisplayID;

/* Wake the display, take the power assertions, and (unless shared mode) dim
 * the built-in panel to black. Safe to call once per session. Never throws. */
- (void)start;

/* Release power assertions, restore the built-in panel brightness, and tear
 * down the input monitor. Safe to call even if start failed or was never
 * called. */
- (void)stop;

@end

NS_ASSUME_NONNULL_END
