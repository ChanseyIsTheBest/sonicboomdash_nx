/* sd_tramp.h -- observe-then-run hooks on IL2CPP methods (sd_tramp.c). */
#ifndef SD_TRAMP_H
#define SD_TRAMP_H
#include <stdint.h>
#include "so_util.h"
#define SD_TRAMP_SLOTS 8
/* `observe` gets the caller's x0..x7 (and x8) BEFORE the method runs; then the
 * method runs unchanged. Verified first: the entry's first two words must equal
 * w0/w1 (the generator's guard) and the first four must be relocatable.
 * 1 = installed, 0 = refused (nothing written). */
int sd_tramp_observe(so_module *mod, uintptr_t target, uint32_t w0, uint32_t w1,
                     void (*observe)(const uint64_t *x), const char *name);
/* The relocation of one instruction at `pc`, for tests: words written to out
 * (up to 4), or -1 if it cannot be moved. */
int sd_tramp_relocate(uint32_t insn, uint64_t pc, uint32_t out[4]);
#endif
