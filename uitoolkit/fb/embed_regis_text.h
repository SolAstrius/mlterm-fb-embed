/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — ReGIS text command renderer.
 *
 * The standalone registobmp tool uses SDL_ttf + fontconfig to
 * implement ReGIS's T'…' text-drawing command. Both are large
 * deps mlterm doesn't otherwise need; embed mode would have to
 * pull them in just for this one ReGIS command.
 *
 * Embed mode instead reuses FreeType (already linked into
 * libuitoolkit for the terminal font) plus the same bundled
 * BDF font the host already extracts for mlterm's own text
 * rendering. Same dep set as the rest of the embed library.
 */

#ifndef ___EMBED_REGIS_TEXT_H__
#define ___EMBED_REGIS_TEXT_H__

#ifdef USE_FB_EMBED

#include <pobl/bl_types.h>
#include "../../tool/registobmp/regis_render.h"

/* Set the font path to use for ReGIS text rendering. Idempotent;
 * called once at scev_term_init_once before any term spawns. The
 * pointer must remain valid for the process lifetime (the host
 * extracts the font once to a temp dir and never moves it). */
void embed_regis_set_font_path(const char *path);

/* Draw a UTF-8 string into `img` at (x, y) (top-left of first
 * glyph) in the given ARGB8888 color, using `size`-pixel font.
 * Returns the total advance width in pixels (caller adds this to
 * pen_x). On any failure (FT init, no font path, glyph missing)
 * returns 0 and paints nothing. */
int embed_regis_draw_text(regis_image_t *img, int x, int y,
                          uint32_t color, int size, const char *utf8);

#endif /* USE_FB_EMBED */
#endif /* ___EMBED_REGIS_TEXT_H__ */
