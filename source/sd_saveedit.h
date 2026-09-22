/* sd_saveedit.h -- Sonic Dash 2 save editor (sd_saveedit.c). */
#ifndef SD_SAVEEDIT_H
#define SD_SAVEEDIT_H
#include <stddef.h>
#include <stdint.h>

/* At boot, before Unity. Writes <game folder>/save_edit.txt (every setting
 * commented out) if it is missing; applies its uncommented settings at every
 * launch, writing the save only when a value differs. 1 changed, 0 nothing
 * to do, -1 refused (the save is untouched). */
int sd_saveedit_apply(void);

/* The file-free core, for tests: save bytes + edit text -> new save bytes. */
int sd_saveedit_transform(const uint8_t *in, size_t in_len, const char *edit_text,
                          uint8_t **out, size_t *out_len, char **dump_text);

/* Hardlight.HLCRC32.Generate(name, Case.Lower): how the save names properties. */
uint32_t sd_saveedit_crc(const char *name);
#endif
