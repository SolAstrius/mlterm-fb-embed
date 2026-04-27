/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-dumper — drive an embed-mode mlterm with a stream of VT bytes
 * and dump the resulting framebuffer as a PPM. Test harness for the
 * fb-embed fork.
 *
 * Strategy: spawn mlterm's standard main_loop (which sets up the
 * screen manager, terminal, fonts, the works), passing -e <command>
 * so the child program writes the VT stream we want rendered. Pump
 * the event source ourselves N times via the embed API's
 * non-blocking pump, then dump our buffer as PPM.
 *
 * Why -e instead of pushing bytes directly: mlterm's own init path
 * creates the ui_screen + ui_window + display tree we need to
 * actually render cells into the buffer. Skipping that init means
 * re-implementing it. -e <cat> piping our input is far less code.
 *
 * Usage:
 *   fb-dumper [-c COLS] [-r ROWS] [-o OUT.ppm] [-f INPUT.vt]
 *             [-d MS] [-e CMD]
 *
 *   -c COLS    grid columns       (default 80)
 *   -r ROWS    grid rows          (default 24)
 *   -o PATH    output PPM path    (default stdout)
 *   -f PATH    pipe this file's contents into a `cat` -e child
 *              (mutually exclusive with -e)
 *   -e CMD     command to run inside mlterm    (default: cat -)
 *   -d MS      pump for this many milliseconds (default 250)
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "../../main/main_loop.h"
#include "../../uitoolkit/ui_event_source.h"
#include "../../uitoolkit/ui_screen_manager.h"
#include "../../uitoolkit/fb/ui_fb_embed.h"

#define DEFAULT_COLS  80
#define DEFAULT_ROWS  24
#define CELL_PX_W     8
#define CELL_PX_H     16
#define DEFAULT_PUMP_MS 250

/* PPM (P6) writer — see README for format rationale. */
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
  int pump_ms = DEFAULT_PUMP_MS;
  const char *out_path = NULL;
  const char *in_path = NULL;
  const char *exec_cmd = NULL;

  int opt;
  while ((opt = getopt(argc, argv, "c:r:o:f:e:d:h")) != -1) {
    switch (opt) {
      case 'c': cols = atoi(optarg); break;
      case 'r': rows = atoi(optarg); break;
      case 'o': out_path = optarg; break;
      case 'f': in_path = optarg; break;
      case 'e': exec_cmd = optarg; break;
      case 'd': pump_ms = atoi(optarg); break;
      case 'h':
      default:
        fprintf(stderr,
            "usage: %s [-c COLS] [-r ROWS] [-o OUT.ppm]\n"
            "          [-f INPUT.vt | -e CMD] [-d MS]\n",
            argv[0]);
        return opt == 'h' ? 0 : 2;
    }
  }
  if (cols <= 0 || rows <= 0 || cols > 1000 || rows > 1000) {
    fprintf(stderr, "bad grid dimensions %dx%d\n", cols, rows);
    return 2;
  }
  if (in_path && exec_cmd) {
    fprintf(stderr, "-f and -e are mutually exclusive\n");
    return 2;
  }

  /* Allocate + attach the host buffer before any uitoolkit call. */
  int width  = cols * CELL_PX_W;
  int height = rows * CELL_PX_H;
  uint32_t *buf = calloc((size_t)width * height, sizeof(uint32_t));
  if (!buf) { perror("calloc"); return 1; }
  if (ui_fb_embed_attach(buf, width, height, width) != 0) {
    fprintf(stderr, "ui_fb_embed_attach failed\n");
    return 1;
  }

  /* Build the argv we'll hand to main_loop_init. mlterm's option
   * parser eats these; -e CMD spawns CMD inside the PTY. We always
   * pass --geometry so the cell grid matches what we asked for. */
  char geom[32];
  snprintf(geom, sizeof(geom), "%dx%d", cols, rows);
  /* Default child is `cat -` which echoes stdin → PTY → mlterm. */
  const char *default_cat = "/bin/cat";
  const char *e_arg = exec_cmd ? exec_cmd : default_cat;

  /* If the user gave -f INPUT, redirect our stdin from that file
   * before exec — `cat -` then dumps the file via the PTY. */
  if (in_path) {
    if (!freopen(in_path, "rb", stdin)) {
      perror(in_path);
      return 1;
    }
  }

  char *ml_argv[] = {
    (char *)"mlterm-fb-dumper",
    (char *)"--geometry", geom,
    (char *)"-e", (char *)e_arg,
    NULL,
  };
  int ml_argc = (int)(sizeof(ml_argv) / sizeof(ml_argv[0])) - 1;

  if (!main_loop_init(ml_argc, ml_argv)) {
    fprintf(stderr, "main_loop_init failed\n");
    return 1;
  }

  /* main_loop_init prepares the screen manager but doesn't actually
   * open any screens — that's the first thing main_loop_start does.
   * We bypass main_loop_start (its blocking event loop is what we
   * replaced with the embed pump) and call startup directly. */
  if (ui_screen_manager_startup() == 0) {
    fprintf(stderr, "ui_screen_manager_startup failed\n");
    return 1;
  }

  /* Pump until the time budget runs out. Each iteration drains
   * any data the child wrote, lets mlterm parse + render into our
   * buffer. 1 ms sleep between pumps so we don't burn CPU and so
   * the child has a chance to actually produce data. */
  struct timeval start, now;
  gettimeofday(&start, NULL);
  for (;;) {
    ui_fb_embed_pump();
    gettimeofday(&now, NULL);
    long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
                      (now.tv_usec - start.tv_usec) / 1000L;
    if (elapsed_ms >= pump_ms) break;
    usleep(1000);
  }

  /* Dump. */
  FILE *out = out_path ? fopen(out_path, "wb") : stdout;
  if (!out) { perror(out_path); return 1; }
  if (write_ppm(out, buf, width, height, width) != 0) {
    fprintf(stderr, "write_ppm failed\n");
    return 1;
  }
  if (out != stdout) fclose(out);

  main_loop_final();
  ui_fb_embed_detach();
  free(buf);
  return 0;
}
