/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — ReGIS T'…' text via FreeType.
 * See embed_regis_text.h for the rationale.
 */

#include "embed_regis_text.h"

#ifdef USE_FB_EMBED

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pobl/bl_debug.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/* --- statics --- */

static FT_Library ft_lib;
static FT_Face    ft_face;
static const char *font_path;
static int        current_size;

static int ensure_face_loaded(int size) {
  if (!font_path) return 0;

  if (!ft_lib) {
    if (FT_Init_FreeType(&ft_lib) != 0) {
      bl_msg_printf("embed_regis_text: FT_Init_FreeType failed\n");
      ft_lib = NULL;
      return 0;
    }
  }

  if (!ft_face) {
    if (FT_New_Face(ft_lib, font_path, 0, &ft_face) != 0) {
      bl_msg_printf("embed_regis_text: FT_New_Face(%s) failed\n", font_path);
      ft_face = NULL;
      return 0;
    }
  }

  /* Bitmap-strike fonts (BDF / PCF / .otb) only support a fixed set
   * of pixel sizes. Cozette has exactly one strike at 6×13 — try
   * FT_Select_Size first. For scalable fonts, FT_Set_Pixel_Sizes. */
  if (size != current_size) {
    if (ft_face->num_fixed_sizes > 0) {
      FT_Select_Size(ft_face, 0);
    } else {
      FT_Set_Pixel_Sizes(ft_face, 0, size);
    }
    current_size = size;
  }

  return 1;
}

/* Decode one UTF-8 codepoint from `s`. Advances `*pos` past the
 * consumed bytes. Returns the codepoint (or 0xFFFD on bad input). */
static uint32_t utf8_decode(const char *s, size_t *pos) {
  unsigned char c = (unsigned char)s[*pos];
  uint32_t cp;
  int extra;

  if (c < 0x80)         { cp = c;        extra = 0; }
  else if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
  else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
  else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
  else { (*pos)++; return 0xFFFD; }

  (*pos)++;
  while (extra-- > 0) {
    unsigned char x = (unsigned char)s[*pos];
    if ((x & 0xc0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (x & 0x3f);
    (*pos)++;
  }
  return cp;
}

/* Blit a single rendered glyph onto img. Glyph bitmap can be 1-bit
 * mono (BDF) or 8-bit grayscale (TTF/OTF) — handle both. */
static void blit_glyph(regis_image_t *img, int dst_x, int dst_y,
                       uint32_t color, FT_Bitmap *bm) {
  int sw = (int)bm->width;
  int sh = (int)bm->rows;

  for (int sy = 0; sy < sh; sy++) {
    int dy = dst_y + sy;
    if (dy < 0 || dy >= img->h) continue;
    for (int sx = 0; sx < sw; sx++) {
      int dx = dst_x + sx;
      if (dx < 0 || dx >= img->w) continue;

      int set = 0;
      if (bm->pixel_mode == FT_PIXEL_MODE_MONO) {
        unsigned char byte = bm->buffer[sy * bm->pitch + (sx >> 3)];
        set = (byte >> (7 - (sx & 7))) & 1;
      } else if (bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
        /* Threshold gray to mono — ReGIS pen color has no alpha
         * channel anyway, and mixing colors with the underlying
         * canvas requires a blend mode the format doesn't define.
         * 0x80 splits the gradient roughly down the middle. */
        set = bm->buffer[sy * bm->pitch + sx] >= 0x80;
      }

      if (set) {
        img->pixels[dy * img->w + dx] = color;
      }
    }
  }
}

/* --- public --- */

void embed_regis_set_font_path(const char *path) {
  font_path = path;
  /* Force re-load on next draw if the path changed mid-process —
   * unlikely in our use, but cheap. */
  if (ft_face) {
    FT_Done_Face(ft_face);
    ft_face = NULL;
    current_size = 0;
  }
}

int embed_regis_draw_text(regis_image_t *img, int x, int y,
                          uint32_t color, int size, const char *utf8) {
  if (!img || !utf8 || !*utf8) {
    fprintf(stderr, "[regis_text] called with %s\n",
            !img ? "NULL img" : (!utf8 ? "NULL utf8" : "empty utf8"));
    return 0;
  }
  if (!ensure_face_loaded(size)) {
    fprintf(stderr, "[regis_text] ensure_face_loaded(size=%d) failed; "
                    "font_path=%s, ft_lib=%p, ft_face=%p\n",
            size, font_path ? font_path : "(null)",
            (void *)ft_lib, (void *)ft_face);
    return 0;
  }
  fprintf(stderr, "[regis_text] draw '%s' at (%d,%d) size=%d color=0x%08x\n",
          utf8, x, y, size, color);

  /* y in ReGIS is the BASELINE position, FreeType's bitmap_top is
   * the distance from baseline UP to the top of the glyph bitmap.
   * Top edge of the glyph in canvas coords = y - bitmap_top. */

  size_t pos = 0;
  size_t len = strlen(utf8);
  int total_advance = 0;

  while (pos < len) {
    uint32_t cp = utf8_decode(utf8, &pos);
    if (FT_Load_Char(ft_face, cp, FT_LOAD_RENDER) != 0) continue;

    FT_GlyphSlot g = ft_face->glyph;
    blit_glyph(img,
               x + total_advance + g->bitmap_left,
               y - g->bitmap_top,
               color, &g->bitmap);
    total_advance += (int)(g->advance.x >> 6);
  }

  return total_advance;
}

#endif /* USE_FB_EMBED */
