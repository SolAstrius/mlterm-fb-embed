/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-dumper — drive an embed-mode mlterm with a stream of VT bytes
 * and dump the resulting framebuffer as a PPM. Test harness for the
 * fb-embed fork: a known input → a binary-comparable output.
 *
 * Only built when configured with --enable-fb-embed. Links against
 * the same uitoolkit/fb objects mlterm-fb itself uses, plus vtemu.
 *
 * Usage:
 *   fb-dumper [-c COLS] [-r ROWS] [-o OUT.ppm] [-f INPUT]
 *
 * If -f is omitted, reads VT bytes from stdin until EOF. If -o is
 * omitted, writes PPM to stdout. Defaults to 80x24 grid.
 *
 * Example — render the boot demo we used in the jexer prototype:
 *   printf '\033[2J\033[H\033[1;36mhello\033[0m\r\n' | \
 *     ./fb-dumper -o hello.ppm
 *
 * Example — diff embed-mode output against a golden capture:
 *   fb-dumper -f tests/inputs/colors.vt -o /tmp/out.ppm
 *   cmp /tmp/out.ppm tests/golden/colors.ppm
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vt_term.h>
#include <vt_term_manager.h>

#include "../../uitoolkit/fb/ui_fb_embed.h"

#define DEFAULT_COLS 80
#define DEFAULT_ROWS 24
#define CELL_PX_W    8   /* matches uitoolkit/fb default pcf cell. */
#define CELL_PX_H    16

static int read_all(FILE *fp, unsigned char **out_buf, size_t *out_len) {
  size_t cap = 4096, len = 0;
  unsigned char *buf = malloc(cap);
  if (!buf) return -1;
  for (;;) {
    if (len == cap) {
      cap *= 2;
      unsigned char *nb = realloc(buf, cap);
      if (!nb) { free(buf); return -1; }
      buf = nb;
    }
    size_t n = fread(buf + len, 1, cap - len, fp);
    len += n;
    if (n == 0) {
      if (feof(fp)) break;
      free(buf);
      return -1;
    }
  }
  *out_buf = buf;
  *out_len = len;
  return 0;
}

/* PPM (P6) is the simplest "real" image format: ASCII header
 * followed by raw RGB triples. Every image viewer reads it; cmp(1)
 * works for byte-exact regression. */
static int write_ppm(FILE *fp, const uint32_t *pixels, int w, int h, int stride) {
  if (fprintf(fp, "P6\n%d %d\n255\n", w, h) < 0) return -1;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint32_t argb = pixels[y * stride + x];
      unsigned char rgb[3] = {
        (unsigned char)((argb >> 16) & 0xFF),
        (unsigned char)((argb >>  8) & 0xFF),
        (unsigned char)( argb        & 0xFF),
      };
      if (fwrite(rgb, 1, 3, fp) != 3) return -1;
    }
  }
  return 0;
}

int main(int argc, char *argv[]) {
  int cols = DEFAULT_COLS;
  int rows = DEFAULT_ROWS;
  const char *out_path = NULL;
  const char *in_path = NULL;

  int opt;
  while ((opt = getopt(argc, argv, "c:r:o:f:h")) != -1) {
    switch (opt) {
      case 'c': cols = atoi(optarg); break;
      case 'r': rows = atoi(optarg); break;
      case 'o': out_path = optarg; break;
      case 'f': in_path = optarg; break;
      case 'h':
      default:
        fprintf(stderr, "usage: %s [-c COLS] [-r ROWS] [-o OUT.ppm] [-f INPUT.vt]\n",
                argv[0]);
        return opt == 'h' ? 0 : 2;
    }
  }
  if (cols <= 0 || rows <= 0 || cols > 1000 || rows > 1000) {
    fprintf(stderr, "bad grid dimensions %dx%d\n", cols, rows);
    return 2;
  }

  int width  = cols * CELL_PX_W;
  int height = rows * CELL_PX_H;
  uint32_t *buf = calloc((size_t)width * height, sizeof(uint32_t));
  if (!buf) { perror("calloc"); return 1; }

  if (ui_fb_embed_attach(buf, width, height, width) != 0) {
    fprintf(stderr, "ui_fb_embed_attach failed\n");
    return 1;
  }

  /* Build a vt_term that owns the parser + cell grid. The fb
   * backend's display layer (now reading our buffer) renders
   * whatever this term holds. Argument order matches the prototype
   * in vtemu/vt_term.h exactly; integer enums get zero values
   * (= "default"), pointer args get NULL. */
  vt_term_t *term = vt_term_new(
      "xterm-256color", cols, rows,
      /*tab_size=*/8, /*log_size=*/64,
      /*encoding=*/0, /*is_auto_encoding=*/0,
      /*use_auto_detect=*/0, /*logging_vt_seq=*/0,
      /*policy=*/0, /*col_size_a=*/1,
      /*use_char_combining=*/1, /*use_multi_col_char=*/1,
      /*use_ctl=*/1, /*bidi_mode=*/0, /*bidi_separators=*/NULL,
      /*use_dynamic_comb=*/0, /*bs_mode=*/0,
      /*vertical_mode=*/0, /*use_local_echo=*/0,
      /*win_name=*/NULL, /*icon_name=*/NULL,
      /*use_ansi_colors=*/1, /*alt_color_mode=*/0,
      /*use_ot_layout=*/0, /*cursor_style=*/0,
      /*ignore_broadcasted_chars=*/0, /*use_locked_title=*/0);
  if (!term) {
    fprintf(stderr, "vt_term_new failed\n");
    return 1;
  }

  /* Slurp input bytes — file or stdin. */
  unsigned char *script;
  size_t script_len;
  FILE *in = in_path ? fopen(in_path, "rb") : stdin;
  if (!in) { perror(in_path); return 1; }
  if (read_all(in, &script, &script_len) != 0) {
    fprintf(stderr, "read failed\n");
    return 1;
  }
  if (in != stdin) fclose(in);

  /* Push bytes into the parser, then drive one render iteration. */
  vt_term_write(term, script, script_len);
  ui_fb_embed_pump();

  free(script);

  /* Dump. */
  FILE *out = out_path ? fopen(out_path, "wb") : stdout;
  if (!out) { perror(out_path); return 1; }
  if (write_ppm(out, buf, width, height, width) != 0) {
    fprintf(stderr, "write_ppm failed\n");
    return 1;
  }
  if (out != stdout) fclose(out);

  vt_term_destroy(term);
  ui_fb_embed_detach();
  free(buf);
  return 0;
}
