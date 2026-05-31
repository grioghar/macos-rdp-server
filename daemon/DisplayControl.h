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
 *  2. Privacy blanking — covers the BUILT-IN physical display with an opaque
 *     black borderless window at shielding level so a bystander at the Mac can't
 *     watch the remote session. The VIRTUAL display (which the remote captures)
 *     is never touched. Skipped entirely in shared mode.
 *
 *  3. Return control to the desk user — a global input monitor removes the
 *     blanking overlay the moment the desk user physically touches the Mac
 *     (mouse move / key / click). It stays unblanked for the rest of the
 *     session (see DisplayControl.m for rationale).
 *
 * macOS single-session constraint: macOS has ONE GUI login session, so we can't
 * show the login window locally while the remote keeps the unlocked desktop on
 * the same session. "Lock the local screen" is therefore implemented as privacy
 * BLANKING of the built-in display only — not a real screen lock.
 *
 * Shared mode (env RDP_SHARED_MODE=="1"): skip blanking so the desk user and
 * remote user both see and control the desktop simultaneously.
 */
@interface DisplayControl : NSObject

/* virtualDisplayID is the display the remote session captures and MUST NOT be
 * blanked. Every other active display that is built-in is blanked for privacy. */
- (instancetype)initWithVirtualDisplayID:(CGDirectDisplayID)virtualDisplayID;

/* Wake the display, take the power assertions, and (unless shared mode) blank
 * the built-in display. Safe to call once per session. Never throws. */
- (void)start;

/* Release power assertions, remove any blanking overlay, and tear down the
 * input monitor. Safe to call even if start failed or was never called. */
- (void)stop;

@end

NS_ASSUME_NONNULL_END
