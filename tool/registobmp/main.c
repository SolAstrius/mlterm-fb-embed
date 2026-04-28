/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*- */

/* fb-embed (downstream fork): this file builds two ways.
 *
 * Standalone tool (default):
 *   The original mlterm registobmp CLI — argv-parses an .rgs file,
 *   uses SDL2 for the canvas, optionally SDL_ttf + fontconfig for
 *   text rendering, writes a .bmp via SDL_SaveBMP. Builds against
 *   the libsdl2-dev system package. Behaviour unchanged from
 *   upstream.
 *
 * Embed library (USE_FB_EMBED):
 *   Same interpreter, but every SDL/SDL_ttf/fontconfig touch is
 *   #ifdef'd out and replaced with a tiny portable shim. Exposes
 *   regis_render_file(path, &out_image) so libuitoolkit can call
 *   the ReGIS interpreter in-process for embed hosts that don't
 *   ship a separate registobmp binary. Text rendering is a no-op
 *   in embed mode (T'...' is consumed but not painted) — text is
 *   rare in real ReGIS streams; lifting SDL_ttf would pull the
 *   whole font-rendering stack into the .so for a tiny payoff.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h> /* atoi */
#include <stdint.h>
#include <math.h>

#ifdef USE_FB_EMBED
#include "regis_render.h"
#else
#include <SDL.h>
#ifdef USE_SDLTTF
#include <SDL_ttf.h>
#endif
#ifdef USE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif
#endif

#include <pobl/bl_def.h>

#ifdef USE_FB_EMBED
/* Shim: replace SDL_Surface with our own portable struct, and
 * the few SDL helpers the interpreter calls. The interpreter
 * itself is unchanged — it accesses regis via the pixel_at()
 * macro which works on any struct with `pixels` + `w`. */
#define SDL_Surface regis_image_t
typedef struct { int x, y, w, h; } SDL_Rect;

static SDL_Surface *embed_create_surface(int width, int height) {
  SDL_Surface *s = (SDL_Surface *)malloc(sizeof(*s));
  if (!s) return NULL;
  s->pixels = (uint32_t *)calloc((size_t)width * height, sizeof(uint32_t));
  if (!s->pixels) { free(s); return NULL; }
  s->w = width;
  s->h = height;
  return s;
}

static void embed_free_surface(SDL_Surface *s) {
  if (!s) return;
  free(s->pixels);
  free(s);
}

/* SDL_FillRect with rect==NULL means "fill whole surface" — only
 * usage in this file. */
static void embed_fill_rect(SDL_Surface *s, SDL_Rect *rect, uint32_t color) {
  (void)rect;
  size_t n = (size_t)s->w * s->h;
  for (size_t i = 0; i < n; i++) s->pixels[i] = color;
}

/* SDL_BlitSurface for the resize() path: copy a top-left sub-rect
 * from src to dst. dstrect with x=y=0 means "place at origin". */
static void embed_blit_surface(SDL_Surface *src, SDL_Rect *srect,
                               SDL_Surface *dst, SDL_Rect *drect) {
  (void)drect;
  int rw = srect->w;
  int rh = srect->h;
  for (int y = 0; y < rh; y++) {
    memcpy(dst->pixels + (size_t)y * dst->w,
           src->pixels + (size_t)y * src->w,
           (size_t)rw * sizeof(uint32_t));
  }
}

#define SDL_CreateRGBSurface(flags, w, h, depth, rm, gm, bm, am) \
        embed_create_surface(w, h)
#define SDL_FillRect(s, r, c)        embed_fill_rect(s, r, c)
#define SDL_BlitSurface(s, sr, d, dr) embed_blit_surface(s, sr, d, dr)
#define SDL_FreeSurface(s)           embed_free_surface(s)
#define SDL_SWSURFACE 0
#endif /* USE_FB_EMBED */

#define pixel_at(x, y) (((u_int32_t *)regis->pixels)[(y)*regis->w + (x)])
#define REGIS_RGB(r, g, b) \
  (0xff000000 | (((r)*255 / 100) << 16) | (((g)*255 / 100) << 8) | ((b)*255 / 100))
#define MAGIC_COLOR 0x0

#if 0
#define __TEST
#endif

/* --- static variables --- */

static int pen_x;
static int pen_y;
static int pen_x_stack[10];
static int pen_y_stack[10];
static int pen_stack_count;
static int circle_center;
/* XXX 3/4 */
static int fontsize = 18 * 3 / 4;
static SDL_Surface *regis;

static u_int32_t fg_color = 0xff000000;
static u_int32_t bg_color = 0xffffffff;
static u_int32_t color_tbl[] = {
    REGIS_RGB(0, 0, 0),    /* BLACK */
    REGIS_RGB(20, 20, 80), /* BLUE */
    REGIS_RGB(80, 13, 13), /* RED */
    REGIS_RGB(20, 80, 20), /* GREEN */
    REGIS_RGB(80, 20, 80), /* MAGENTA */
    REGIS_RGB(20, 80, 80), /* CYAN */
    REGIS_RGB(80, 80, 20), /* YELLOW */
    REGIS_RGB(53, 53, 53), /* GRAY 50% */
    REGIS_RGB(26, 26, 26), /* GRAY 25% */
    REGIS_RGB(33, 33, 60), /* BLUE* */
    REGIS_RGB(60, 26, 26), /* RED* */
    REGIS_RGB(33, 60, 33), /* GREEN* */
    REGIS_RGB(60, 33, 60), /* MAGENTA* */
    REGIS_RGB(33, 60, 60), /* CYAN*/
    REGIS_RGB(60, 60, 33), /* YELLOW* */
    REGIS_RGB(80, 80, 80)  /* GRAY 75% */
};

/* --- static functions --- */

static void help(void) {
  fprintf(stderr, "registobmp [src: regis text file] [dst: bitmap file]\n");
  fprintf(stderr, " (Environment variables) REGIS_FONT=[font path]\n");
}

static void init_state(void) {
  pen_stack_count = 0;
  circle_center = 0;
}

static void resize(int width, int height) {
  SDL_Surface *new_regis;

  new_regis = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32, 0x00FF0000, 0x0000FF00,
                                   0x000000FF, 0xFF000000);
  SDL_FillRect(new_regis, NULL, bg_color);

  if (regis) {
    SDL_Rect rect;

    rect.x = 0;
    rect.y = 0;
    rect.w = (regis->w > width) ? width : regis->w;
    rect.h = (regis->h > height) ? height : regis->h;

    SDL_BlitSurface(regis, &rect, new_regis, NULL);
  }

  regis = new_regis;
}

static void draw_line(int start_x, int start_y, int end_x, int end_y, int color) {
  int cx;
  int cy;
  int dx;
  int dy;
  int sx;
  int sy;
  int err;
  int n;

  cx = start_x;
  cy = start_y;

  dx = end_x < cx ? cx - end_x : end_x - cx;
  dy = end_y < cy ? cy - end_y : end_y - cy;

  sx = cx < end_x ? 1 : -1;
  sy = cy < end_y ? 1 : -1;
  err = dx - dy;

  for (n = 0; n < 1000; n++) {
    int err2;

    pixel_at(cx, cy) = color;
    if (cx == end_x && cy == end_y) {
      return;
    }

    err2 = 2 * err;

    if (err2 > -dy) {
      err = err - dy;
      cx = cx + sx;
    }

    if (err2 < dx) {
      err = err + dx;
      cy = cy + sy;
    }
  }
}

static void draw_circle(int x, int y, int r, int color) {
  int d;
  int dh;
  int dd;
  int cx;
  int cy;

  d = 1 - r;
  dh = 3;
  dd = 5 - 2 * r;
  cy = r;

  for (cx = 0; cx <= cy; cx++) {
    if (d < 0) {
      d += dh;
      dh += 2;
      dd += 2;
    } else {
      d += dd;
      dh += 2;
      dd += 4;
      cy--;
    }

    pixel_at(cy + x, cx + y) = color;
    pixel_at(cx + x, cy + y) = color;
    pixel_at(-cx + x, cy + y) = color;
    pixel_at(-cy + x, cx + y) = color;
    pixel_at(-cy + x, -cx + y) = color;
    pixel_at(-cx + x, -cy + y) = color;
    pixel_at(cx + x, -cy + y) = color;
    pixel_at(cy + x, -cx + y) = color;
  }
}

static int parse_options(char **options, /* 10 elements */
                         char **cmd) {
  u_int num_options;
  int skip_count;
  int inside_bracket;
  char *end;

  num_options = 0;
  inside_bracket = skip_count = 0;
  end = (*cmd);

  while (1) {
    if (*end == '(') {
      skip_count++;
    } else if (*end == ')') {
      if (skip_count == 0) {
        *end = '\0';
        if (options) {
          options[num_options++] = *cmd;
          options[num_options] = NULL;
        }
        *cmd = end + 1;

        return 1;
      } else {
        skip_count--;
      }
    } else if (*end == '[') {
      inside_bracket = 1;
    } else if (*end == ']') {
      inside_bracket = 0;
    } else if (*end == ',') {
      if (skip_count == 0 && !inside_bracket) {
        *end = '\0';
        if (num_options < 8 && options) {
          options[num_options++] = *cmd;
        }
        *cmd = end + 1;
      }
    } else if (*end == '\0') {
      if (options) {
        options[num_options++] = *cmd;
        options[num_options] = NULL;
      }
      *cmd = NULL;

      return 1;
    }

    end++;
  }
}

static int parse_coordinate(int *x, int *y, char **cmd) {
  char *p;
  char *xstr;
  char *ystr;

  xstr = *cmd;

  if (!(p = strchr(xstr, ']'))) {
    return 0;
  }

  *p = '\0';
  *cmd = p + 1;

  if (!(ystr = strchr(xstr, ',')) || *ystr == '\0') {
    ystr = "+0";
  } else {
    *(ystr++) = '\0';
  }

  if (*xstr == '\0') {
    xstr = "+0";
  }

  if (*xstr == '+' || *xstr == '-') {
    if ((*x = atoi(xstr) + pen_x) < 0) {
      *x = 0;
    }
  } else {
    *x = atoi(xstr);
  }

  if (*ystr == '+' || *ystr == '-') {
    if ((*y = atoi(ystr) + pen_y) < 0) {
      *y = 0;
    }
  } else {
    *y = atoi(ystr);
  }

  return 1;
}

static char *command_screen(char *cmd) {
  char *options[10];
  int count;

  if (*cmd == '(') {
    cmd++;
    if (!parse_options(options, &cmd)) {
      return cmd - 1;
    }
  } else {
    return cmd;
  }

  for (count = 0; options[count]; count++) {
    char *option;

    option = options[count];

    if (*option == 'I') {
      if (*(++option) == '(') {
        /* XXX Not standard command. */

        int red;
        int green;
        int blue;

        if (sscanf(option + 1, "R%dG%dB%d", &red, &green, &blue) == 3) {
          bg_color = 0xff000000 | (red << 16) | (green << 8) | blue;
        }
      } else {
        bg_color = color_tbl[atoi(option + 1)];
      }
    } else if (*option == 'E') {
      SDL_FillRect(regis, NULL, bg_color);
    } else if (*option == 'C') {
      /*
       * C0: turns the output cursor off.
       * C1: turns the output cursor on.
       */
    }
  }

  return cmd;
}

static char *parse_quoted_text(char *text, char quote) {
  int skip_count;

  skip_count = 0;

  while (1) {
    if (*text == quote) {
      if (*(text + 1) == quote) {
        memmove(text, text + 1, strlen(text + 1) + 1);
        skip_count++;
      } else {
        *(text++) = '\0';

        return text + skip_count;
      }
    } else if (*text == '\0') {
      return NULL;
    }

    text++;
  }
}

static char *command_text(char *cmd) {
#ifdef USE_FB_EMBED
  /* fb-embed: no font stack in the embed library. Walk past the
   * options + quoted string so the parser keeps going, but don't
   * paint anything. SDL_ttf + fontconfig would otherwise pull a
   * 100-MiB chain of deps into the .so for a feature most ReGIS
   * streams (vector geometry from VAX/PDP-era OSes) don't use. */
  if (*cmd == '(') {
    char *options[10];
    cmd++;
    if (!parse_options(options, &cmd)) {
      return cmd - 1;
    }
  }
  char quote = *cmd;
  char *text = cmd + 1;
  if ((quote != '\'' && quote != '"') || !(cmd = parse_quoted_text(text, quote))) {
    return cmd;
  }
  return cmd;
}
#else
  static int no_font;
  char quote;
  char *text;
#ifdef USE_SDLTTF
  static TTF_Font *font;
  SDL_Surface *image;
  SDL_Color color;
  SDL_Rect image_rect;
  SDL_Rect rect;
#endif

  if (*cmd == '(') {
    char *options[10];
    int count;

    cmd++;
    if (!parse_options(options, &cmd)) {
      return cmd - 1;
    }

    for (count = 0; options[count]; count++) {
      char *option;

      option = options[count];

      if (*option == 'S') {
        int size;

        if (*(++option) == '[') {
          int w;
          int h;

          option++;
          if (parse_coordinate(&w, &h, &option)) {
            /* XXX 3/4 */
            size = (h < w * 2 ? h : w * 2) * 3 / 4;

            if (size != fontsize) {
              fontsize = size;

#ifdef USE_SDLTTF
              if (font) {
                TTF_CloseFont(font);
                font = NULL;
              }
#endif
            }
          }
        } else {
          int height_tbl[] = {10,  20,  30,  45,  60,  75,  90,  105, 120,
                              135, 150, 165, 180, 195, 210, 225, 240};
          int width_tbl[] = {9, 9, 18, 27, 36, 45, 54, 63, 72, 81, 90, 99, 108, 117, 126, 135, 144};
          int idx;

          idx = atoi(option);
          if (0 <= idx && idx <= 16) {
            /* XXX 3/4 */
            size = (height_tbl[idx] < width_tbl[idx] * 2 ? height_tbl[idx] : width_tbl[idx] * 2) *
                   3 / 4;

            if (size != fontsize) {
              fontsize = size;

#ifdef USE_SDLTTF
              if (font) {
                TTF_CloseFont(font);
                font = NULL;
              }
#endif
            }
          }
        }
      }
    }
  }

  quote = *cmd;
  text = cmd + 1;

  if ((quote != '\'' && quote != '"') || !(cmd = parse_quoted_text(text, quote))) {
    return cmd;
  }

  if (no_font) {
    return cmd;
  }

#ifdef USE_SDLTTF
  if (!font) {
    static char *font_file;

    if (!font_file) {
      if (!(font_file = getenv("REGIS_FONT"))) {
#if defined(USE_FONTCONFIG)
        FcPattern *pat;
        FcPattern *mat = NULL;
        FcResult result;

        FcInit();

#ifdef USE_WIN32API
        pat = FcNameParse("Courier");
#else
        pat = FcNameParse("monospace");
#endif
        FcConfigSubstitute(0, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        mat = FcFontMatch(0, pat, &result);
        FcPatternDestroy(pat);

        if (FcPatternGetString(mat, FC_FILE, 0, (FcChar8**)&font_file) != FcResultMatch ||
            !(font_file = strdup(font_file))) {
          font_file = "arial.ttf";
        }

        FcPatternDestroy(mat);
#elif defined(USE_WIN32GUI)
        font_file = "c:\\Windows\\Fonts\\arial.ttf";
#else
        font_file = "arial.ttf";
#endif
      }

      TTF_Init();
    }

    if (!(font = TTF_OpenFont(font_file, fontsize))) {
      fprintf(stderr, "Not found %s.\n", font_file);
    }

    TTF_SetFontHinting(font, TTF_HINTING_MONO);

    if (!font) {
      no_font = 1;
      TTF_Quit();

      return cmd;
    }
  }

  color.r = (fg_color >> 16) & 0xff;
  color.g = (fg_color >> 8) & 0xff;
  color.b = fg_color & 0xff;
#if SDL_MAJOR_VERSION > 1
  color.a = (fg_color >> 24) & 0xff;
#endif
  if ((image = TTF_RenderUTF8_Blended(font, text, color))) {
    image_rect.x = 0;
    image_rect.y = 0;
    image_rect.w = image->w;
    image_rect.h = image->h;
    rect.x = pen_x;
    rect.y = pen_y;
    rect.w = 0;
    rect.h = 0;

    SDL_BlitSurface(image, &image_rect, regis, &rect);

    pen_x += image->w;
  }
#endif

  return cmd;
}
#endif /* USE_FB_EMBED else */

static char *command_w(char *cmd) {
  char *options[10];
  int count;

  if (*cmd == '(') {
    cmd++;
    if (!parse_options(options, &cmd)) {
      return cmd - 1;
    }
  } else {
    return cmd;
  }

  for (count = 0; options[count]; count++) {
    char *option;

    option = options[count];

    if (*option == 'I') {
      fg_color = color_tbl[atoi(option + 1)];
    }
  }

  return cmd;
}

static char *command_position(char *cmd) {
  int new_x;
  int new_y;

  if (*(cmd++) == '[') {
    if (!parse_coordinate(&new_x, &new_y, &cmd)) {
      return cmd - 1;
    }

    pen_x = new_x;
    pen_y = new_y;

    return cmd;
  } else {
    return cmd - 1;
  }
}

static char *command_vector(char *cmd) {
  int new_x;
  int new_y;

  if (*cmd == '[') {
    cmd++;
    if (!parse_coordinate(&new_x, &new_y, &cmd)) {
      return cmd - 1;
    }

    draw_line(pen_x, pen_y, new_x, new_y, fg_color);

    pen_x = new_x;
    pen_y = new_y;
  } else if (*cmd == '(' && *(cmd + 2) == ')') {
    if (*(cmd + 1) == 'B') {
      if (pen_stack_count < 10) {
        pen_x_stack[pen_stack_count] = pen_x;
        pen_y_stack[pen_stack_count] = pen_y;
      }

      pen_stack_count++;
    } else if (*(cmd + 1) == 'S') {
      if (pen_stack_count < 10) {
        pen_x_stack[pen_stack_count] = -1;
      }

      pen_stack_count++;
    } else if (*(cmd + 1) == 'E') {
      pen_stack_count--;

      if (pen_stack_count < 10 && pen_x_stack[pen_stack_count] >= 0) {
        draw_line(pen_x, pen_y, pen_x_stack[pen_stack_count], pen_y_stack[pen_stack_count],
                  fg_color);
        pen_x = pen_x_stack[pen_stack_count];
        pen_y = pen_y_stack[pen_stack_count];
      }
    }

    cmd += 3;
  }

  return cmd;
}

static char *command_circle(char *cmd) {
  if (*cmd == '[') {
    int x;
    int y;
    int r;

    cmd++;
    if (!parse_coordinate(&x, &y, &cmd)) {
      return cmd - 1;
    }

    if (x == pen_x) {
      r = abs(y - pen_y);
    } else if (y == pen_y) {
      r = abs(x - pen_x);
    } else {
      r = sqrt(pow(abs(x - pen_x), 2) + pow(abs(y - pen_y), 2));
    }

    if (circle_center) {
      draw_circle(x, y, r, fg_color);
    } else {
      draw_circle(pen_x, pen_y, r, fg_color);
    }
  } else if (strncmp(cmd, "(C)", 3) == 0) {
    circle_center = 1;
    cmd += 3;
  }

  return cmd;
}

static void fill(int x, int y) {
  if (y >= 1 && pixel_at(x, y - 1) != MAGIC_COLOR) {
    pixel_at(x, y - 1) = MAGIC_COLOR;
    fill(x, y - 1);
  }

  if (y < regis->h - 1 && pixel_at(x, y + 1) != MAGIC_COLOR) {
    pixel_at(x, y + 1) = MAGIC_COLOR;
    fill(x, y + 1);
  }

  if (x >= 1 && pixel_at(x - 1, y) != MAGIC_COLOR) {
    pixel_at(x - 1, y) = MAGIC_COLOR;
    fill(x - 1, y);
  }

  if (x < regis->w + 1 && pixel_at(x + 1, y) != MAGIC_COLOR) {
    pixel_at(x + 1, y) = MAGIC_COLOR;
    fill(x + 1, y);
  }
}

static char *command(char *cmd);

static char *command_fill(char *cmd) {
  char *options[10];
  int orig_fg_color;
  int x;
  int y;

  if (*cmd == '(') {
    cmd++;
    if (!parse_options(options, &cmd)) {
      return cmd - 1;
    }
  } else {
    return cmd;
  }

  orig_fg_color = fg_color;
  fg_color = MAGIC_COLOR;
  while (*(options[0] = command(options[0])))
    ;

  fg_color = orig_fg_color;

  for (y = 0; y < regis->h; y++) {
    for (x = 0; x < regis->w; x++) {
      if (pixel_at(x, y) == MAGIC_COLOR && y < 799 && pixel_at(x, y + 1) != MAGIC_COLOR) {
        fill(x, y + 1);

        for (; y < regis->h; y++) {
          for (x = 0; x < regis->w; x++) {
            if (pixel_at(x, y) == MAGIC_COLOR) {
              pixel_at(x, y) = fg_color;
            }
          }
        }

        break;
      }
    }
  }

  return cmd;
}

static char *command(char *cmd) {
  static char *(*command)(char *);

  switch (*cmd) {
    char *orig_cmd;

    default:
      orig_cmd = cmd;
      if (!command || (cmd = (*command)(orig_cmd)) == orig_cmd) {
        return orig_cmd + 1;
      } else {
        return cmd;
      }

    case 'S':
      command = command_screen;
      break;

    case 'T':
      command = command_text;
      break;

    case 'W':
      command = command_w;
      break;

    case 'P':
      command = command_position;
      break;

    case 'v':
    case 'V':
      command = command_vector;
      break;

    case 'C':
      command = command_circle;
      break;

    case 'F':
      command = command_fill;
      break;
  }

  init_state();
  return (*command)(cmd + 1);
}

#ifdef __TEST

static void test_parse_options(void) {
  char cmd[] = "S(V(W(I(R,P))),S1)";
  char *p;
  char *options[10];
  int count;

  p = cmd + 2;
  parse_options(options, &p);

  for (count = 0; options[count]; count++) {
    fprintf(stderr, "%s %s\n", options[count], p);
  }
}

static void test_parse_coordinate(void) {
  char cmd[] = "[100,200][300,400][+200,-300][+200][,-300]";
  char *p;
  int x;
  int y;

  pen_x = 100;
  pen_y = 200;
  p = cmd;
  while (*(p++) == '[') {
    parse_coordinate(&x, &y, &p);

    fprintf(stderr, "%d %d\n", x, y);
  }
  pen_x = 0;
  pen_y = 0;
}

static void test_parse_quoted_text(void) {
  char text1[] = "\"don't\"";
  char text2[] = "\'\"stop\"\'";
  char text3[] = "'don''t'";
  char text4[] = "\"ditto (\"\")\"";

  fprintf(stderr, "%s => ", text1);
  parse_quoted_text(&text1[1], text1[0]);
  fprintf(stderr, "%s\n", text1 + 1);

  fprintf(stderr, "%s => ", text2);
  parse_quoted_text(&text2[1], text2[0]);
  fprintf(stderr, "%s\n", text2 + 1);

  fprintf(stderr, "%s => ", text3);
  parse_quoted_text(&text3[1], text3[0]);
  fprintf(stderr, "%s\n", text3 + 1);

  fprintf(stderr, "%s => ", text4);
  parse_quoted_text(text4 + 1, *text4);
  fprintf(stderr, "%s\n", text4 + 1);
}

#endif

/* --- global functions --- */

#ifdef USE_FB_EMBED
/* fb-embed entry. Reads `path` (a .rgs source file), runs the
 * interpreter, hands back an ARGB8888 image. Caller frees out->pixels
 * via regis_image_free(). Returns 1 on success, 0 on failure.
 *
 * Initial canvas size is 800×480, matching the standalone tool's
 * default. The interpreter may grow this via the S(A...) command. */
int regis_render_file(const char *path, regis_image_t *out) {
  FILE *fp;
  char line[256];
  char *cmd;

  if (!out || !path) return 0;
  out->pixels = NULL;
  out->w = out->h = 0;

  if (!(fp = fopen(path, "r"))) {
    return 0;
  }

  /* Reset interpreter state (file-scope statics). */
  pen_x = pen_y = 0;
  pen_stack_count = 0;
  circle_center = 0;
  fg_color = 0xff000000;
  bg_color = 0xffffffff;
  if (regis) {
    embed_free_surface(regis);
    regis = NULL;
  }
  resize(800, 480);

  if ((cmd = fgets(line, sizeof(line), fp))) {
    char *p;
    if (*cmd == '\x1b' && (p = strchr(cmd, 'p'))) {
      cmd = p + 1;
    }
    do {
      while (*(cmd = command(cmd)))
        ;
    } while ((cmd = fgets(line, sizeof(line), fp)));
  }
  fclose(fp);

  if (!regis) return 0;

  /* Hand pixel ownership to the caller. The interpreter wrote
   * 0xAARRGGBB host-endian uint32s; on a little-endian host that
   * lays out as B,G,R,A bytes in memory. embed_imgloader expects
   * RGBA byte order (R at byte 0). Swap R↔B in place. */
  size_t n = (size_t)regis->w * regis->h;
  for (size_t i = 0; i < n; i++) {
    uint32_t p = regis->pixels[i];
    uint32_t a = (p >> 24) & 0xff;
    uint32_t r = (p >> 16) & 0xff;
    uint32_t g = (p >>  8) & 0xff;
    uint32_t b =  p        & 0xff;
    regis->pixels[i] = (a << 24) | (b << 16) | (g << 8) | r;
  }

  out->pixels = regis->pixels;
  out->w = regis->w;
  out->h = regis->h;

  /* Don't free regis->pixels — caller owns now. Free the wrapper. */
  free(regis);
  regis = NULL;

  return 1;
}

void regis_image_free(regis_image_t *img) {
  if (!img) return;
  free(img->pixels);
  img->pixels = NULL;
  img->w = img->h = 0;
}

#else  /* USE_FB_EMBED — standalone tool from here on */

int main(int argc, char **argv) {
  FILE *fp;
  char line[256];
  char *cmd;

#if defined(_WIN32) && defined(USE_FONTCONFIG)
  if (!getenv("FONTCONFIG_PATH") && !getenv("FONTCONFIG_FILE")) {
    /*
     * See fontconfig-x.x.x/src/fccfg.c
     * (DllMain(), FcConfigFileExists(), FcConfigGetPath() and FcConfigFilename())
     *
     * [commant in DllMain]
     * If the fontconfig DLL is in a "bin" or "lib" subfolder, assume it's a Unix-style
     * installation tree, and use "etc/fonts" in there as FONTCONFIG_PATH.
     * Otherwise use the folder where the DLL is as FONTCONFIG_PATH.
     *
     * [comment in FcConfigFileExists]
     * make sure there's a single separator
     * (=> If FONTCONFIG_PATH="", FONTCONFIG_FILE="/...")
     */
    putenv("FONTCONFIG_PATH=.");
  }
#endif

#ifdef __TEST
  test_parse_options();
  test_parse_coordinate();
  test_parse_quoted_text();
#endif

  if (argc != 3) {
    help();

    return -1;
  }

  if (!(fp = fopen(argv[1], "r"))) {
    return -1;
  }

#if 0
  SDL_Init(SDL_INIT_EVERYTHING);
#endif

  resize(800, 480);

  if ((cmd = fgets(line, sizeof(line), fp))) {
    char *p;

    if (*cmd == '\x1b' && (p = strchr(cmd, 'p'))) {
      cmd = p + 1;
    }

    do {
      while (*(cmd = command(cmd)))
        ;
    } while ((cmd = fgets(line, sizeof(line), fp)));
  }

  fclose(fp);

  SDL_SaveBMP(regis, argv[2]);

  return 0;
}

#endif /* USE_FB_EMBED */
