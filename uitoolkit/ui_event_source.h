/* -*- c-basic-offset:2; tab-width:2; indent-tabs-mode:nil -*- */

#ifndef __UI_EVENT_SOURCE_H__
#define __UI_EVENT_SOURCE_H__

void ui_event_source_init(void);

void ui_event_source_final(void);

int ui_event_source_process(void);

#ifdef USE_FB_EMBED
/* fb-embed (downstream fork) — non-blocking single-shot pump for
 * embedding hosts that drive their own UI loop. See ui_event_source.c. */
void ui_event_source_pump_once_nonblock(void);
#endif

int ui_event_source_add_fd(int fd, void (*handler)(void));

void ui_event_source_remove_fd(int fd);

#endif
