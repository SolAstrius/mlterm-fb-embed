/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * Each gui's ui.h (fb/win32/console/wayland) #defines the X11 modifier
 * masks (ShiftMask, ControlMask, ...) so the shared uitoolkit input code
 * compiles on non-X backends. Those names collide with struct members in
 * <windows.h> (e.g. PROCESS_POWER_THROTTLING_STATE.ControlMask), so any
 * TU that needs windows.h after ui.h fails to parse it.
 *
 * Include this immediately before <windows.h> in files that use windows.h
 * but none of the masks. (The win32 GUI itself avoids the clash by
 * including windows.h before its mask defines.)
 */

#undef ShiftMask
#undef LockMask
#undef ControlMask
#undef Mod1Mask
#undef Mod2Mask
#undef Mod3Mask
#undef Mod4Mask
#undef Mod5Mask
#undef Button1Mask
#undef Button2Mask
#undef Button3Mask
#undef Button4Mask
#undef Button5Mask
