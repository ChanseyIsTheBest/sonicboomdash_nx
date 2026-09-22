/* error.h -- error handler
 *
 * Copyright (C) 2021 fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __ERROR_H__
#define __ERROR_H__

void fatal_error(const char *fmt, ...) __attribute__((noreturn));

/* First-boot progress console (asset pack build). end() must run before the
 * engine creates its EGL context. */
void startup_status_begin(const char *message);
void startup_status_update(const char *message);
void startup_status_end(void);

#endif
