/* sd_tramp.c -- observe-then-run hooks on IL2CPP methods.
 *
 *   target:  ldr x16, #8 ; br x16 ; .quad thunk_N        (the first 16 bytes, replaced)
 *   thunk_N: save x0-x8, d0-d7 ; bl observer ; restore ; br trampoline_N
 *   trampoline_N (in the cave): the four displaced instructions, relocated,
 *            then ldr x16, #8 ; br x16 ; .quad target+16
 *
 * The thunk BRANCHES into the trampoline rather than calling it, so while the
 * method runs no frame of ours is on the stack: the method returns straight to
 * its caller, and a managed exception it throws unwinds exactly as before.
 *
 * THE CAVE is the unused tail of libil2cpp's last executable page:
 * so_finalize maps every page an RX segment touches as RX, so [segment end,
 * page end) is executable and referenced by nothing (3328 bytes here).
 * Writes go through so_patch_code, as every other patch in this port does.
 * ------------------------------------------------------------------------- */
#include <stdint.h>
#include <string.h>
#include <switch.h>

#include "so_util.h"
#include "util.h"
#include "sd_tramp.h"

int so_patch_code(void *dst, const void *src, size_t len);

void *sd_tramp_orig[SD_TRAMP_SLOTS];                     /* read by the thunks */
static void (*s_obs[SD_TRAMP_SLOTS])(const uint64_t *x);
static int s_used;
static uintptr_t s_cave, s_cave_end;

/* Called by every thunk with the saved registers: x[0..7] = x0..x7, x[8] = x8. */
void sd_tramp_dispatch(const uint64_t *x, uint64_t slot) {
  if (slot < SD_TRAMP_SLOTS && s_obs[slot]) s_obs[slot](x);
}

/* The thunks. Frame: [sp] x29,x30 | +0x10 x0..x7 | +0x50 x8 | +0x60 d0..d7. */
#define SD_THUNK(n) \
  ".balign 16\n.global sd_thunk_" #n "\nsd_thunk_" #n ":\n" \
  "  stp x29, x30, [sp, #-0xa0]!\n  mov x29, sp\n" \
  "  stp x0, x1, [sp, #0x10]\n  stp x2, x3, [sp, #0x20]\n  stp x4, x5, [sp, #0x30]\n  stp x6, x7, [sp, #0x40]\n" \
  "  str x8, [sp, #0x50]\n" \
  "  stp d0, d1, [sp, #0x60]\n  stp d2, d3, [sp, #0x70]\n  stp d4, d5, [sp, #0x80]\n  stp d6, d7, [sp, #0x90]\n" \
  "  add x0, sp, #0x10\n  mov x1, #" #n "\n  bl sd_tramp_dispatch\n" \
  "  ldp d6, d7, [sp, #0x90]\n  ldp d4, d5, [sp, #0x80]\n  ldp d2, d3, [sp, #0x70]\n  ldp d0, d1, [sp, #0x60]\n" \
  "  ldr x8, [sp, #0x50]\n" \
  "  ldp x6, x7, [sp, #0x40]\n  ldp x4, x5, [sp, #0x30]\n  ldp x2, x3, [sp, #0x20]\n  ldp x0, x1, [sp, #0x10]\n" \
  "  ldp x29, x30, [sp], #0xa0\n" \
  "  adrp x16, sd_tramp_orig\n  add x16, x16, :lo12:sd_tramp_orig\n  ldr x16, [x16, #" #n "*8]\n  br x16\n"
__asm__(".text\n" SD_THUNK(0) SD_THUNK(1) SD_THUNK(2) SD_THUNK(3) SD_THUNK(4) SD_THUNK(5) SD_THUNK(6) SD_THUNK(7));
extern char sd_thunk_0[], sd_thunk_1[], sd_thunk_2[], sd_thunk_3[], sd_thunk_4[], sd_thunk_5[], sd_thunk_6[], sd_thunk_7[];
static void *const k_thunks[SD_TRAMP_SLOTS] = { sd_thunk_0, sd_thunk_1, sd_thunk_2, sd_thunk_3,
                                                 sd_thunk_4, sd_thunk_5, sd_thunk_6, sd_thunk_7 };

int sd_tramp_relocate(uint32_t insn, uint64_t pc, uint32_t out[4]) {
  if ((insn & 0x9F000000u) == 0x90000000u) {             /* ADRP -> movz/movk of the page */
    const unsigned rd = insn & 31;
    int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFFu) << 2) | ((insn >> 29) & 3u));
    imm = (imm << 43) >> 43;                              /* sign-extend 21 bits */
    const uint64_t page = (pc & ~0xFFFull) + (uint64_t)(imm * 4096);
    for (unsigned hw = 0; hw < 4; hw++)
      out[hw] = (hw ? 0xF2800000u : 0xD2800000u) | (hw << 21) | ((uint32_t)((page >> (16 * hw)) & 0xFFFF) << 5) | rd;
    return 4;
  }
  if ((insn & 0x9F000000u) == 0x10000000u) return -1;    /* ADR */
  if ((insn & 0x7C000000u) == 0x14000000u) return -1;    /* B, BL */
  if ((insn & 0xFF000010u) == 0x54000000u) return -1;    /* B.cond */
  if ((insn & 0x7E000000u) == 0x34000000u) return -1;    /* CBZ, CBNZ */
  if ((insn & 0x7E000000u) == 0x36000000u) return -1;    /* TBZ, TBNZ */
  if ((insn & 0x3B000000u) == 0x18000000u) return -1;    /* LDR/LDRSW/PRFM (literal) */
  if ((insn & 0xFE000000u) == 0xD6000000u) return -1;    /* BR, BLR, RET, ERET */
  if ((insn & 0xFFFFF01Fu) == 0xD503201Fu && insn != 0xD503201Fu) return -1;  /* hint space other than NOP (BTI/PAC) */
  out[0] = insn;
  return 1;
}

static int cave_init(so_module *mod) {
  if (s_cave) return 1;
  for (int i = 0; i < mod->phnum; i++) {
    const Elf64_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
    const uintptr_t end = (uintptr_t)mod->load_virtbase + p->p_vaddr + p->p_memsz;
    const uintptr_t start = (end + 15) & ~(uintptr_t)15, page_end = (end + 0xFFF) & ~(uintptr_t)0xFFF;
    if (page_end - start > s_cave_end - s_cave) { s_cave = start; s_cave_end = page_end; }
  }
  if (s_cave_end - s_cave < 128) { debugPrintf("[tramp] no code cave in the module -- observe hooks unavailable\n"); s_cave = 0; return 0; }
  debugPrintf("[tramp] code cave %p..%p (%u bytes, the tail of the last executable page)\n",
              (void *)s_cave, (void *)s_cave_end, (unsigned)(s_cave_end - s_cave));
  return 1;
}

int sd_tramp_observe(so_module *mod, uintptr_t target, uint32_t w0, uint32_t w1,
                     void (*observe)(const uint64_t *x), const char *name) {
  if (s_used >= SD_TRAMP_SLOTS) { debugPrintf("[tramp] %s: no free slot\n", name); return 0; }
  if (!cave_init(mod)) return 0;
  const uint32_t *code = (const uint32_t *)target;
  if (code[0] != w0 || code[1] != w1) {
    debugPrintf("[tramp] %s: guard mismatch (%08x %08x, want %08x %08x) -- NOT hooked\n", name, code[0], code[1], w0, w1);
    return 0;
  }
  uint32_t buf[24]; int n = 0;
  for (int i = 0; i < 4; i++) {
    const int k = sd_tramp_relocate(code[i], (uint64_t)(target + 4u * (unsigned)i), &buf[n]);
    if (k < 0) { debugPrintf("[tramp] %s: instruction %d (%08x) cannot be relocated -- NOT hooked\n", name, i, code[i]); return 0; }
    n += k;
  }
  uintptr_t at = s_cave;
  if (((at + 4u * (unsigned)n) & 7) != 0) buf[n++] = 0xD503201Fu;   /* nop: keep the literal 8-aligned */
  buf[n++] = 0x58000050u;                                 /* ldr x16, #8 */
  buf[n++] = 0xD61F0200u;                                 /* br  x16    */
  const uint64_t back = (uint64_t)target + 16;
  memcpy(&buf[n], &back, 8); n += 2;
  if (at + 4u * (unsigned)n > s_cave_end) { debugPrintf("[tramp] %s: code cave full\n", name); return 0; }
  if (so_patch_code((void *)at, buf, 4u * (unsigned)n) != 0) return 0;
  s_cave = (at + 4u * (unsigned)n + 15) & ~(uintptr_t)15;

  const int slot = s_used++;
  s_obs[slot] = observe;
  sd_tramp_orig[slot] = (void *)at;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  uint32_t jump[4] = { 0x58000050u, 0xD61F0200u, 0, 0 };
  const uint64_t th = (uint64_t)(uintptr_t)k_thunks[slot];
  memcpy(&jump[2], &th, 8);
  if (so_patch_code((void *)target, jump, sizeof jump) != 0) { s_obs[slot] = NULL; return 0; }
  debugPrintf("[tramp] %s observed (slot %d, trampoline %p, %d words)\n", name, slot, (void *)at, n);
  return 1;
}
