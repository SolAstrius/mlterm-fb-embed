/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — in-process ReGIS interpreter API.
 *
 * The standalone registobmp tool's interpreter (tool/registobmp/main.c)
 * is built as a static library when --enable-fb-embed; this header
 * exposes the entry point libuitoolkit needs to render ReGIS escape
 * sequences without the fork+execve dance the upstream model uses.
 *
 * Text command (T'...') is a no-op in embed mode — embed builds skip
 * SDL_ttf + fontconfig to keep the .so self-contained. Vector geometry
 * (lines, circles, fills, palette, position) all work.
 */

#ifndef ___REGIS_RENDER_H__
#define ___REGIS_RENDER_H__

#ifdef USE_FB_EMBED

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Output canvas. pixels is RGBA8888 byte order (R at byte 0 on
 * little-endian) so it can be handed straight to libuitoolkit's
 * embed image loader, which matches what stb_image's stbi_load
 * returns for desired_channels=4. Owned by the caller after
 * regis_render_file() succeeds; release via regis_image_free(). */
typedef struct regis_image {
  uint32_t *pixels;
  int w;
  int h;
} regis_image_t;

int  regis_render_file(const char *path, regis_image_t *out);
void regis_image_free(regis_image_t *img);

#ifdef __cplusplus
}
#endif

#endif /* USE_FB_EMBED */
#endif /* ___REGIS_RENDER_H__ */
