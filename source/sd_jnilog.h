/* sd_jnilog.h -- extensive JNI logging (sd_jnilog.c). */
#ifndef SD_JNILOG_H
#define SD_JNILOG_H
#include <stdarg.h>
#include <stdint.h>
/* kind: 'o' object (iret = pointer), 'i' int/long/bool, 'f' float/double, 'v' void.
 * route: 0 game, 1 sdk-dormant, 2 unity, 3 text/movie, 4 core. `args` is a COPY. */
void sd_jnilog_call(char kind, const void *id, void *recv, int route, va_list args, uint64_t iret, double fret);
void sd_jnilog_field(char kind, const void *id, int is_static, uint64_t iv, double fv, const void *ov);
#endif
