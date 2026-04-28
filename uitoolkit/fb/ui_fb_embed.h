/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — embed entry points.
 *
 * Only present when configured with --enable-fb-embed. Lets a host
 * application (no /dev/fb0, no /dev/input/event*) drive the
 * framebuffer backend's rendering pipeline.
 *
 * Ownership model:
 *   - Host owns the pixel buffer and its lifetime.
 *   - Host calls ui_fb_embed_attach() before any uitoolkit code runs
 *     (i.e. before ui_display_open()) to register the buffer.
 *   - mlterm renders into the buffer in place.
 *   - Host calls ui_fb_embed_detach() at shutdown.
 *
 * Pixel format is fixed: ARGB8888 packed in a host-endian uint32_t,
 * matching what the existing fb backend writes when /dev/fb0 reports
 * a 32 bpp visual. For now embed mode hard-requires this format —
 * other depths can come later if needed.
 *
 * Threading: not safe to call attach/detach concurrent with any
 * other uitoolkit call. Render and event-injection calls are
 * expected from the host's single-threaded UI loop, same model
 * upstream mlterm-fb assumes for its own event source.
 */

#ifndef ___UI_FB_EMBED_H__
#define ___UI_FB_EMBED_H__

#ifdef USE_FB_EMBED

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Attach a host-supplied pixel buffer. Must be called before any
 * other uitoolkit API. `buf` must be at least `width * height * 4`
 * bytes and remain valid until ui_fb_embed_detach(). `stride_px` is
 * the row stride in pixels (for tightly packed buffers, == width).
 *
 * Returns 0 on success, non-zero on failure (typically because
 * mlterm is already initialised).
 */
int ui_fb_embed_attach(uint32_t *buf, int width, int height, int stride_px);

/* Release the buffer reference. The host must not free `buf`
 * before this returns. */
void ui_fb_embed_detach(void);

/* Wire-shape of one input event the host pushes via
 * ui_fb_embed_input(). Three fields, no timeval — the receive_*_event
 * hooks downstream don't use the timestamp and zeroing it costs us a
 * gettimeofday() per event we don't need.
 *
 * Numerically identical to the (type, code, value) trailing triple of
 * Linux's evdev `struct input_event`, so a host on Linux can keep
 * passing EV_KEY/KEY_A/etc. constants. Hosts on Mac/Win/BSD where
 * <linux/input.h> doesn't exist are expected to ship a compatible
 * key-code mapping themselves. */
typedef struct ui_fb_input_event {
  uint16_t type;
  uint16_t code;
  int32_t  value;
} ui_fb_input_event_t;

/* Push one input event into mlterm's event queue. Type/code/value
 * follow the Linux evdev convention (EV_KEY + KEY_*, EV_REL + REL_*,
 * EV_SYN + SYN_REPORT). Lets the host substitute for what upstream
 * mlterm-fb reads from /dev/input/eventN. */
void ui_fb_embed_input(int type, int code, int value);

/* Drive one render+event-pump iteration of the uitoolkit loop.
 * Replaces the upstream "select() on input fds + timer" loop in
 * ui_event_source.c. The host calls this on its own cadence
 * (e.g. once per frame); mlterm processes whatever pending PTY data
 * and queued input events, redraws if dirty. Non-blocking. */
void ui_fb_embed_pump(void);

#ifdef __cplusplus
}
#endif

#endif /* USE_FB_EMBED */
#endif /* ___UI_FB_EMBED_H__ */
