/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — in-process image loader interface.
 * See embed_imgloader.c for the rationale.
 */

#ifndef ___EMBED_IMGLOADER_H__
#define ___EMBED_IMGLOADER_H__

#ifdef USE_FB_EMBED

#include <pobl/bl_types.h>

/* Decode + (optionally) resize an image file in-process via
 * stb_image. Output is RGBA8888 byte order, width*height*4 bytes,
 * malloc'd; caller free()s. desired_w/h == 0 → no resize.
 * Returns 1 on success, 0 on failure. */
int embed_load_image_file(const char *path, u_int desired_w, u_int desired_h,
                          int keep_aspect, u_char **out_image,
                          u_int *out_w, u_int *out_h);

#endif /* USE_FB_EMBED */
#endif /* ___EMBED_IMGLOADER_H__ */
