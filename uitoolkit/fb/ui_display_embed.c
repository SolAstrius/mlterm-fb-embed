/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — display backend for embedded mode.
 *
 * Substitutes for ui_display_{linux,freebsd,wscons,x68kgrf}.c when
 * the build was configured with --enable-fb-embed. Same internal
 * contract: provides open_display(), set_use_console_backscroll(),
 * receive_mouse_event(), receive_key_event(), and the console_id
 * variable that ui_display.c references.
 *
 * The big difference: where the host-OS files own /dev/fb0 and
 * /dev/input/event*, this file owns nothing. The embedding
 * application registers a uint32_t pixel buffer via
 * ui_fb_embed_attach() before any uitoolkit call; open_display()
 * then populates _display from that registration.
 *
 * For input, the embedding application calls ui_fb_embed_input(),
 * which enqueues one event into an in-process ring buffer. The fb
 * event source drains it on its next pump via receive_{key,mouse}_
 * event(). Embed mode is fully in-process, so the former self-pipe
 * (a pipe() whose read end was select()'d) was pure overhead — a
 * lock-free SPSC ring drops both the syscall-per-event and the POSIX
 * pipe/fcntl dependency that Windows lacks.
 *
 * Files compiled in embed mode that DO NOT include this:
 *   - ui_display.c — bracketed with #ifndef USE_FB_EMBED around the
 *     parts that touch stdin / cmap_init / console teardown.
 *
 * This file is #include'd from ui_display.c, same pattern as the
 * upstream per-platform files; it doesn't get its own translation
 * unit.
 */

#include <stdatomic.h>

#include "ui_fb_embed.h"
#include "../ui_event_source.h"   /* ui_event_source_process for the pump */

/* Cross-platform input event constants: just the few we need to
 * route between kbd and mouse channels. Numerically match Linux evdev
 * so hosts on Linux can pass EV_KEY/EV_REL/BTN_MOUSE/KEY_OK
 * directly; hosts on Mac/Win/BSD use the same constants without
 * needing <linux/input.h>. */
#ifndef EV_SYN
#define EV_SYN     0x00
#define EV_KEY     0x01
#define EV_REL     0x02
#endif
#ifndef BTN_MOUSE
#define BTN_MOUSE  0x110
#endif
#ifndef KEY_OK
#define KEY_OK     0x160
#endif

/* --- in-process input queue (replaces the former self-pipe) --- */

/* Single-producer (host thread in ui_fb_embed_input) / single-consumer
 * (pump thread draining via receive_{key,mouse}_event) lock-free ring.
 * Full = drop, the same back-pressure the non-blocking pipe had. */
#define EMBED_RING_CAP 256u   /* power of two */

typedef struct {
  ui_fb_input_event_t buf[EMBED_RING_CAP];
  _Atomic unsigned head;  /* producer advances */
  _Atomic unsigned tail;  /* consumer advances */
} embed_ring_t;

static embed_ring_t kbd_ring;
static embed_ring_t mouse_ring;

static void ring_push(embed_ring_t *r, const ui_fb_input_event_t *ev) {
  unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
  unsigned tail = atomic_load_explicit(&r->tail, memory_order_acquire);
  if (head - tail >= EMBED_RING_CAP) return;  /* full — drop */
  r->buf[head & (EMBED_RING_CAP - 1)] = *ev;
  atomic_store_explicit(&r->head, head + 1, memory_order_release);
}

/* Drain + discard. M0-of-port stub: the linux file's key/mouse
 * translation (~500 lines per direction) will be lifted into a shared
 * helper in a follow-up so embed mode can feed these into vt_term;
 * until then embed-mode events are consumed and dropped, exactly as
 * the old pipe-draining stub did. Returns the count drained. */
static int ring_drain(embed_ring_t *r) {
  unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  unsigned head = atomic_load_explicit(&r->head, memory_order_acquire);
  int n = 0;
  for (; tail != head; tail++) {
    ui_fb_input_event_t ev = r->buf[tail & (EMBED_RING_CAP - 1)];
    (void)ev;  /* discarded for now — see comment above */
    n++;
  }
  atomic_store_explicit(&r->tail, tail, memory_order_release);
  return n;
}

static void ring_reset(embed_ring_t *r) {
  atomic_store_explicit(&r->head, 0, memory_order_relaxed);
  atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
}

/* --- static state --- */

static int console_id = 0;     /* matches the upstream file's variable name */

static struct {
  uint32_t *buf;
  int width;
  int height;
  int stride_px;
  int attached;
} _embed = { NULL, 0, 0, 0, 0 };

/* --- contract required by ui_display.c --- */

static int open_display(u_int depth) {
  if (!_embed.attached) {
    bl_error_printf("ui_fb_embed_attach() must be called before open_display() in embed mode\n");
    return 0;
  }

  ring_reset(&kbd_ring);
  ring_reset(&mouse_ring);

  /* Hand the host buffer over to the existing rendering pipeline.
   * fb and fb_base both point at the host buffer; back_fb is unused
   * (no double-buffering — the host is welcome to double-buffer at
   * its own layer). */
  _display.fb = _display.fb_base = (unsigned char *)_embed.buf;
  _display.fb_fd = -1;
  _display.smem_len = (size_t)_embed.height * _embed.stride_px * sizeof(uint32_t);
  _display.line_length = _embed.stride_px * sizeof(uint32_t);
  _display.xoffset = 0;
  _display.yoffset = 0;
  _display.bytes_per_pixel = 4;
  _display.pixels_per_byte = 1;
  _display.width  = _disp.width  = _embed.width;
  _display.height = _disp.height = _embed.height;
  _disp.depth = 32;

  /* ARGB8888 host-endian. Matches the field layout the upstream
   * Linux file reads from FBIOGET_VSCREENINFO on a typical PC fb. */
  _display.rgbinfo.r_offset = 16; _display.rgbinfo.r_limit = 0;
  _display.rgbinfo.g_offset =  8; _display.rgbinfo.g_limit = 0;
  _display.rgbinfo.b_offset =  0; _display.rgbinfo.b_limit = 0;
  _display.rgbinfo.a_offset = 24; _display.rgbinfo.a_limit = 0;

  /* Embed input arrives through the in-process ring, not an fd. -1
   * tells the event source there is nothing to select() on (the pump
   * drains the ring directly). */
  _display.fd = -1;
  _disp.display = &_display;

  /* Embed mode: no mouse cursor in the rendered buffer (the host
   * draws its own cursor). Skipping the _mouse / _disp_mouse setup
   * also avoids num_opened_displays > 1, which makes the renderer
   * double-paint cells in some paths. */
  _mouse.fd = -1;
#ifdef __linux__
  /* Linux exposes num_opened_displays as a writable variable. On other
   * hosts it is a macro, (MOUSE_IS_INITED ? 2 : 1), which already yields
   * 1 here because _mouse.fd == -1 — so no assignment is needed (or
   * possible). */
  num_opened_displays = 1;
#endif

  return 1;
}

/* No-op — embed mode doesn't own any host console. */
static void set_use_console_backscroll(int use) { (void)use; }

/* receive_*_event: drain the in-process queue. The fd argument (the
 * display's former pipe read end) is now always -1 and ignored. */
static int receive_mouse_event(int fd) { (void)fd; return ring_drain(&mouse_ring); }
static int receive_key_event(int fd)   { (void)fd; return ring_drain(&kbd_ring); }

/* --- public embed API (declared in ui_fb_embed.h) --- */

int ui_fb_embed_attach(uint32_t *buf, int width, int height, int stride_px) {
  if (_embed.attached) return -1;
  if (!buf || width <= 0 || height <= 0 || stride_px < width) return -1;
  _embed.buf = buf;
  _embed.width = width;
  _embed.height = height;
  _embed.stride_px = stride_px;
  _embed.attached = 1;

  /* Reattach path. ui_display_open()'s body only runs once
   * (gated on DISP_IS_INITED); on a second host attach it
   * returns the existing _disp without re-running open_display(),
   * which would otherwise refresh _display.fb to point at the new
   * host buffer. Do it here so the first ui_fb_embed_pump after a
   * reattach paints into the right buffer. */
  if (_disp.display == &_display) {
    ring_reset(&kbd_ring);
    ring_reset(&mouse_ring);
    _display.fb = _display.fb_base = (unsigned char *)buf;
    _display.smem_len = (size_t)height * stride_px * sizeof(uint32_t);
    _display.line_length = stride_px * sizeof(uint32_t);
    _display.width  = _disp.width  = width;
    _display.height = _disp.height = height;
    _display.fd = -1;
  }

  return 0;
}

void ui_fb_embed_detach(void) {
  if (!_embed.attached) return;
  ring_reset(&kbd_ring);
  ring_reset(&mouse_ring);
  _embed.buf = NULL;
  _embed.width = _embed.height = _embed.stride_px = 0;
  _embed.attached = 0;
}

void ui_fb_embed_input(int type, int code, int value) {
  if (!_embed.attached) return;
  ui_fb_input_event_t ev;
  ev.type = (uint16_t)type;
  ev.code = (uint16_t)code;
  ev.value = (int32_t)value;
  /* Route by event type. Key codes (EV_KEY) above BTN_MOUSE / below
   * KEY_OK belong to the mouse channel; everything else is keyboard.
   * Crude but matches what evdev's per-device routing produces in
   * practice. */
  int is_mouse = (ev.type == EV_REL ||
                  (ev.type == EV_KEY && ev.code >= BTN_MOUSE && ev.code < KEY_OK));
  ring_push(is_mouse ? &mouse_ring : &kbd_ring, &ev);
}

/* Forward decl — defined in fb/ui_window.c, only visible inside the
 * fb backend. Advances the DECSCLM smooth-scroll animation by one
 * frame. Cheap when no animation is in flight (one branch). */
extern void ui_fb_smooth_scroll_tick(void);

void ui_fb_embed_pump(void) {
  if (!_embed.attached) return;
  ui_event_source_pump_once_nonblock();
  /* §4.7.8 (VT100 TM): smooth scroll moves data "one scan line in
   * each frame." We treat each pump call as one frame — the host's
   * cadence (typically 60 Hz) becomes the spec's 60 Hz frame rate. */
  ui_fb_smooth_scroll_tick();
}
