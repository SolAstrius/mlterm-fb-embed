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
 * For input, this file holds two pipes (kbd_pipe, mouse_pipe). The
 * embedding application calls ui_fb_embed_input() which writes a
 * single struct input_event into the appropriate pipe. The fb event
 * source then reads it on its next iteration through select(), no
 * different from how the upstream linux file handles
 * /dev/input/event* — so we don't have to duplicate the chunky
 * key/mouse translation logic.
 *
 * Files compiled in embed mode that DO NOT include this:
 *   - ui_display.c — bracketed with #ifndef USE_FB_EMBED around the
 *     parts that touch stdin / cmap_init / console teardown.
 *
 * This file is #include'd from ui_display.c, same pattern as the
 * upstream per-platform files; it doesn't get its own translation
 * unit.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#include "ui_fb_embed.h"
#include "../ui_event_source.h"   /* ui_event_source_process for the pump */

/* Cross-platform input event constants: just the few we need to
 * route between kbd and mouse pipes. Numerically match Linux evdev
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

/* --- static state --- */

static int console_id = 0;     /* matches the upstream file's variable name */

static struct {
  uint32_t *buf;
  int width;
  int height;
  int stride_px;
  int kbd_pipe[2];   /* [0] read end (consumed by receive_key_event),
                      * [1] write end (ui_fb_embed_input writes here). */
  int mouse_pipe[2]; /* same shape, mouse channel. */
  int attached;
} _embed = { NULL, 0, 0, 0, { -1, -1 }, { -1, -1 }, 0 };

/* --- internal helpers --- */

static int make_pipe(int fds[2]) {
  if (pipe(fds) < 0) return -1;
  /* Both ends non-blocking — receive_*_event drains opportunistically,
   * ui_fb_embed_input must not block the host's UI thread. */
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
  fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);
  return 0;
}

/* --- contract required by ui_display.c --- */

static int open_display(u_int depth) {
  if (!_embed.attached) {
    bl_error_printf("ui_fb_embed_attach() must be called before open_display() in embed mode\n");
    return 0;
  }

  if (make_pipe(_embed.kbd_pipe) < 0 || make_pipe(_embed.mouse_pipe) < 0) {
    bl_error_printf("embed: pipe() failed: %s\n", strerror(errno));
    return 0;
  }

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

  _display.fd = _embed.kbd_pipe[0];
  _disp.display = &_display;

  /* Embed mode: no mouse cursor in the rendered buffer (the host
   * draws its own cursor). Skipping the _mouse / _disp_mouse setup
   * also avoids num_opened_displays > 1, which makes the renderer
   * double-paint cells in some paths. */
  _mouse.fd = -1;
  num_opened_displays = 1;

  return 1;
}

/* No-op — embed mode doesn't own any host console. */
static void set_use_console_backscroll(int use) { (void)use; }

/* receive_*_event: drain the pipe and discard. M0-of-port stub —
 * the linux file's translation logic (~500 lines per direction)
 * will be lifted out into a shareable helper in a follow-up commit
 * so embed mode can call it. Until then, embed-mode keystrokes
 * arrive but get dropped on the floor; the dumper test path
 * doesn't exercise input so this is OK to land. */
static int receive_mouse_event(int fd) {
  ui_fb_input_event_t ev;
  int n = 0;
  while (read(fd, &ev, sizeof(ev)) > 0) { n++; }
  return n;
}

static int receive_key_event(int fd) {
  ui_fb_input_event_t ev;
  int n = 0;
  while (read(fd, &ev, sizeof(ev)) > 0) { n++; }
  return n;
}

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
   * host buffer and reopen the input pipes the host previously
   * detached. Do it here so the first ui_fb_embed_pump after a
   * reattach paints into the right buffer. */
  if (_disp.display == &_display) {
    if (make_pipe(_embed.kbd_pipe) < 0 || make_pipe(_embed.mouse_pipe) < 0) {
      bl_error_printf("embed: pipe() reattach failed: %s\n", strerror(errno));
      _embed.attached = 0;
      return -1;
    }
    _display.fb = _display.fb_base = (unsigned char *)buf;
    _display.smem_len = (size_t)height * stride_px * sizeof(uint32_t);
    _display.line_length = stride_px * sizeof(uint32_t);
    _display.width  = _disp.width  = width;
    _display.height = _disp.height = height;
    _display.fd = _embed.kbd_pipe[0];
  }

  return 0;
}

void ui_fb_embed_detach(void) {
  if (!_embed.attached) return;
  for (int i = 0; i < 2; i++) {
    if (_embed.kbd_pipe[i]   >= 0) { close(_embed.kbd_pipe[i]);   _embed.kbd_pipe[i]   = -1; }
    if (_embed.mouse_pipe[i] >= 0) { close(_embed.mouse_pipe[i]); _embed.mouse_pipe[i] = -1; }
  }
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
  int wfd = (ev.type == EV_REL ||
             (ev.type == EV_KEY && ev.code >= BTN_MOUSE && ev.code < KEY_OK))
             ? _embed.mouse_pipe[1]
             : _embed.kbd_pipe[1];
  ssize_t r = write(wfd, &ev, sizeof(ev));
  (void)r;  /* full pipe = host pumping too slowly; drop is fine */
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
