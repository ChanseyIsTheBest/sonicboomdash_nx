/* sd_uitrace.h -- log how each tap resolves into a click (sd_uitrace.c). */
#ifndef SD_UITRACE_H
#define SD_UITRACE_H
#include "so_util.h"
void sd_uitrace_install(so_module *il2cpp);   /* after the input hooks; config.txt ui_trace */
#endif
