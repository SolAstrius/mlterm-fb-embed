/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — in-process image loader.
 *
 * Replaces the fork+execve to mlimgloader/registobmp that the
 * upstream fb backend uses to decode PNG/JPG/GIF/BMP files. Lets
 * an embed host ship one libuitoolkit-built .so/.dylib/.dll with
 * NO accompanying helper binaries — image loading happens in
 * process via stb_image, which is itself a single-header public-
 * domain decoder we already vendor under tool/mlimgloader/.
 *
 * Sixel is handled by ui_imagelib.c's existing BUILTIN_SIXEL path
 * before we ever get called, so this file only handles formats
 * stb_image understands. ReGIS (.rgs) is intentionally NOT handled
 * here yet — the conversion logic still lives inside the standalone
 * registobmp tool's main() and would need to be extracted into a
 * callable function before we can call it in-process. For now
 * embed hosts that need ReGIS should keep using the upstream
 * exec-helper path (and ship registobmp alongside the .so).
 *
 * Wire shape mirrors mlimgloader's stdout protocol:
 *   *out_image:  malloc'd RGBA8888 pixel buffer, w*h*4 bytes
 *   *out_w/*out_h: actual image dimensions after decode + resize
 * Caller free()s out_image when done. NULL return == load failed.
 */

#include "embed_imgloader.h"

#ifdef USE_FB_EMBED

#include <stdlib.h>
#include <string.h>
#include <pobl/bl_debug.h>

/* Single-header lib comes in through here; STB_IMAGE_IMPLEMENTATION
 * is defined once in this translation unit only. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO 0
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "../../tool/mlimgloader/stb_image.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "../../tool/mlimgloader/stb_image_resize2.h"

int embed_load_image_file(const char *path, u_int desired_w, u_int desired_h,
                          int keep_aspect, u_char **out_image,
                          u_int *out_w, u_int *out_h) {
  int w, h, comp;
  unsigned char *raw;

  /* desired_channels=4 → stb_image always returns RGBA8888 byte
   * order (R at byte 0). Matches what mlimgloader writes over the
   * stdout pipe in the upstream fork+exec model. */
  raw = stbi_load(path, &w, &h, &comp, 4);
  if (!raw) {
    bl_msg_printf("embed_imgloader: stbi_load failed for %s: %s\n",
                  path, stbi_failure_reason());
    return 0;
  }

  /* If no resize requested, hand back the raw decoded buffer. */
  if (desired_w == 0 || desired_h == 0 ||
      ((u_int)w == desired_w && (u_int)h == desired_h)) {
    *out_image = raw;
    *out_w = (u_int)w;
    *out_h = (u_int)h;
    return 1;
  }

  /* Resize requested. keep_aspect adjusts the target dims so the
   * source aspect ratio is preserved within the requested box. */
  u_int dst_w = desired_w;
  u_int dst_h = desired_h;
  if (keep_aspect) {
    double src_aspect = (double)w / (double)h;
    double dst_aspect = (double)dst_w / (double)dst_h;
    if (src_aspect > dst_aspect) {
      dst_h = (u_int)(dst_w / src_aspect);
    } else {
      dst_w = (u_int)(dst_h * src_aspect);
    }
    if (dst_w == 0) dst_w = 1;
    if (dst_h == 0) dst_h = 1;
  }

  unsigned char *resized = (unsigned char *)malloc((size_t)dst_w * dst_h * 4);
  if (!resized) {
    stbi_image_free(raw);
    return 0;
  }

  stbir_resize_uint8_srgb(raw, w, h, w * 4,
                          resized, dst_w, dst_h, dst_w * 4,
                          STBIR_RGBA);
  stbi_image_free(raw);

  *out_image = resized;
  *out_w = dst_w;
  *out_h = dst_h;
  return 1;
}

#endif /* USE_FB_EMBED */
