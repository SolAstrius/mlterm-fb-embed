/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*-
 *
 * fb-embed (downstream fork) — pulls the lifted ReGIS interpreter
 * from tool/registobmp/main.c into libuitoolkit.a.
 *
 * The interpreter source is the SAME file the standalone registobmp
 * tool builds, but its USE_FB_EMBED branches replace the SDL +
 * SDL_ttf + fontconfig touches with portable shims and add a
 * regis_render_file() public entry. We #include it from this
 * wrapper so libuitoolkit picks up exactly one TU containing the
 * embed-side ReGIS implementation, with no source duplication.
 */

#ifdef USE_FB_EMBED

/* Force the embed branch even though Makefile flags should already
 * pass it — defensive in case this file ever gets pulled into a
 * non-embed build. */
#ifndef USE_FB_EMBED
#define USE_FB_EMBED
#endif

#include "../../tool/registobmp/main.c"

#endif /* USE_FB_EMBED */
