/* error.c -- error handler
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util.h"
#include "error.h"

/* First-boot progress console for the asset pack (asset_pack.c reports through
 * startup_status_update). From fruitninjaclassic_nx's error.c. It uses the libnx
 * console, like fatal_error(), and owns the default window only until
 * startup_status_end() -- which runs before the engine creates its EGL
 * context, so it never collides with the game. Every update is also logged,
 * so debug.log keeps the full trace. */
static int s_status_console = 0;
static void status_draw(const char *title, int percent) {
  printf("\x1b[2J\x1b[H");                        /* clear + home */
  printf("\n\n  Sonic Dash 2: Sonic Boom - first boot\n");
  printf("  -------------------------------------\n\n");
  printf("  %s\n\n", title);
  if (percent >= 0) {
    if (percent > 100) percent = 100;
    char bar[41]; int fill = percent * 40 / 100;
    for (int i = 0; i < 40; i++) bar[i] = (i < fill) ? '#' : '-';
    bar[40] = 0;
    printf("  [%s] %3d%%\n\n", bar, percent);
  }
  printf("  This runs once. Please wait...\n");
  consoleUpdate(NULL);
}
static void status_show(const char *m) {
  if (!m) return;
  /* flatten newlines for the log; a trailing "NN%" becomes the progress bar */
  char one[160]; size_t j = 0;
  for (size_t i = 0; m[i] && j < sizeof(one) - 1; i++) one[j++] = (m[i] == '\n' ? ' ' : m[i]);
  one[j] = 0; while (j && one[j - 1] == ' ') one[--j] = 0;
  static char last[160];
  if (strcmp(one, last)) { strncpy(last, one, sizeof last); last[sizeof last - 1] = 0;
    debugPrintf("[pack] %s\n", one); }
  int percent = -1;
  char title[160]; strncpy(title, one, sizeof title); title[sizeof title - 1] = 0;
  size_t n = strlen(title);
  if (n && title[n - 1] == '%') {
    size_t k = n - 1; while (k > 0 && title[k - 1] >= '0' && title[k - 1] <= '9') k--;
    percent = atoi(title + k);
    while (k > 0 && title[k - 1] == ' ') k--;
    title[k] = 0;
  }
  if (!s_status_console) { consoleInit(NULL); s_status_console = 1; }
  status_draw(title, percent);
}
void startup_status_begin(const char *m)  { status_show(m); }
void startup_status_update(const char *m) { status_show(m); }
void startup_status_end(void) {
  if (s_status_console) { consoleExit(NULL); s_status_console = 0; }
}

void fatal_error(const char *fmt, ...) {
  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);

  consoleInit(NULL);

  va_list list;
  va_start(list, fmt);
  vprintf(fmt, list);
  va_end(list);

  printf("\n\nPress A to exit.");

  consoleUpdate(NULL);

  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 keys = padGetButtonsDown(&pad);
    if (keys & HidNpadButton_A) break;
  }

  consoleExit(NULL);
  exit(1);
}
