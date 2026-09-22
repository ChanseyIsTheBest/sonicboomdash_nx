/* sd_gc.h -- Boehm GC stop-the-world globals, in libil2cpp.so.
 *
 * DIFFERENT BINARY, DIFFERENT METHOD. Everything in sd_offsets.h is a
 * libunity.so RVA derived against the version-matched reference pair. These
 * four are not. They live in libil2cpp.so, which is the AOT-compiled GAME --
 * unique to this title and this build. No symbol kit exists for it and none
 * ever will, so these were derived directly from your binary by walking the
 * call graph (tools/derive_gc_bridge.py).
 *
 * WHY THEY MATTER MORE THAN ANYTHING ELSE IN THE TREE
 * Boehm's stop-the-world suspends every mutator thread with pthread_kill() and
 * waits on a semaphore for each to acknowledge. Switch never delivers POSIX
 * signals, so the handler never runs, the ack never arrives, and the FIRST
 * COLLECTION HANGS FOREVER. That is the classic stuck-at-boot wall, and it
 * does not announce itself -- there is no error, the game simply stops.
 *
 * libc_shim.c's pthread_kill_gc() emulates the handler by posting the ack
 * semaphore itself. To do that it needs these addresses.
 *
 * HOW THEY WERE FOUND -- Sonic Dash 2's libil2cpp.so
 * (BuildID f36040ea187e8a089e0acb9a287bed8c113e9357, matches its metadata)
 *   pthread_kill call sites: 0x0fd39c0 and 0x0fd3c20. Walking back for the
 *   last write to w1 (the `sig` argument) gives `ldr w1, [x24, #off]` naming
 *   0x25b428c and 0x25b4290 -- adjacent, as Boehm allocates them -- with
 *   GC_retry_signals one word below at 0x25b4288. The same shape as SD1.
 *   The single sem_wait lies BETWEEN the two sites (0x0fd39c0 < 0x0fd3b48 <
 *   0x0fd3c20), so the first is GC_suspend_all's and the second
 *   GC_restart_all's.
 *
 *   The semaphore: sem_post at 0x0fd38b0 / 0x0fd38fc and sem_init at
 *   0x0fd3d00 all reference 0x27d36d0.
 *
 *   The tool again reported sem_wait's argument as "not from a global": at
 *   0x0fd3b3c the address is built in x20 (adrp/add) and moved to x0 one
 *   instruction before the call, outside its backward window. Disassembly
 *   confirms sem_wait uses the SAME 0x27d36d0, in suspend_restart_barrier's
 *   ack loop. So the semaphore is pinned by four independent call sites.
 *
 * THE SIGNAL NUMBERS ARE -1 IN THE FILE. Do not hardcode them. Boehm assigns
 * GC_sig_suspend / GC_sig_thr_restart from SIGRTMIN+n during GC_init, so the
 * shipped .data values are placeholders. READ THEM AT RUNTIME, after the
 * runtime has initialised. A shim that captures them at load time captures -1
 * and will never match the signal it is asked to deliver.
 */
#ifndef SD_GC_H
#define SD_GC_H

#include <stdint.h>
#include <stddef.h>

/* RVAs into libil2cpp.so (mapped size 0x27d6780). */
#define SD_GC_START_ACK_OFF      0x025b4288u  /* GC_retry_signals, .data, i32 */
#define SD_GC_SUSPEND_SIG_OFF    0x025b428cu  /* GC_sig_suspend,      .data, i32 */
#define SD_GC_RESTART_SIG_OFF    0x025b4290u  /* GC_sig_thr_restart,  .data, i32 */
#define SD_GC_ACK_SEM_OFF        0x027d36d0u  /* GC_suspend_ack_sem,  .bss, sem_t */

/* Shipped values, for sanity-checking a runtime read:
 *     GC_retry_signals   = 1
 *     GC_sig_suspend     = -1   <- assigned at GC_init
 *     GC_sig_thr_restart = -1   <- assigned at GC_init
 * If your runtime read still shows -1, GC_init has not run yet and the bridge
 * must not be armed. */
#define SD_GC_RETRY_SIGNALS_INITIAL   1

/* Range guard. battd_nx records that reusing another game's GC offsets was
 * caught exactly here, by a bounds check. Keep it. */
#define SD_GC_IL2CPP_MAPPED_SIZE 0x27d6780u
#define SD_GC_OFF_VALID(o)  ((o) < SD_GC_IL2CPP_MAPPED_SIZE)


/* ------------------------------------------------------------------------
 * GC_threads[] -- needed to do the suspend handler's OTHER job
 *
 * Posting the ack is only half of what the never-delivered signal handler was
 * supposed to do. The other half is recording, for each stopped thread, where
 * its stack currently ends and what its registers hold -- because that is what
 * GC_push_all_stacks scans to find roots.
 *
 * Skip it and the collector marks from an incomplete root set. Objects that are
 * still live, but referenced only from an unpublished thread, get reclaimed.
 * That failure does NOT show up at collection time; it shows up much later as a
 * container with a non-zero count and a NULL element, walked by unrelated code.
 * battd_nx's logs record exactly that shape.
 *
 * Derived by tools/derive_gc_threads.py from GC_suspend_all's loop and
 * GC_push_all_stacks' "sp not set!" abort path. All 9 guard words below were
 * read from SD2's libil2cpp.so. Globals keep SD1's spacing: stop count,
 * then the ack semaphore at +0x10, then GC_threads[] at +0x30.
 *
 * The record layout (0x0/0x8/0x10/0x18, 256 buckets) is IDENTICAL to the one
 * battd_nx derived from a different game on a different Unity version. That is
 * expected -- it is bdwgc's own struct, not Unity's -- and it is a useful
 * cross-check that the derivation landed on real Boehm code.
 * ------------------------------------------------------------------------ */
#define SD_GC_THREADS_OFF        0x027d36f0u  /* GC_threads[]  */
#define SD_GC_THREADS_BUCKETS    256
#define SD_GC_THR_NEXT_OFF       0x0
#define SD_GC_THR_ID_OFF         0x8
#define SD_GC_THR_LASTSTOP_OFF   0x10         /* stop_info.last_stop_count */
#define SD_GC_THR_STACKPTR_OFF   0x18         /* stop_info.stack_ptr       */
#define SD_GC_STOP_COUNT_OFF     0x027d36c0u  /* GC_stop_count */

/* Room for 33 saved words (x0..x28, fp, lr, sp, pc), 16-byte aligned. The
 * saved block is placed BELOW the stopped thread's sp and stack_ptr is pointed
 * at it, so the collector scans the registers as roots too. */
#define SD_GC_REGSAVE_BYTES      0x120

/* Instruction guards. Every one verified against your binary (9/9). The bridge
 * checks these ONCE before arming and disables itself loudly on any mismatch,
 * because the cost of being wrong here is silent heap corruption rather than a
 * crash you can read. */
typedef struct { uint32_t off; uint32_t word; const char *what; } SdGcGuard;

/* Guards on the GLOBALS themselves, not just the thread table. Each load
 * below encodes one global's offset in its immediate, so if these match, the
 * four addresses this bridge dereferences are confirmed in the running binary.
 * (An earlier revision left this table as zero-word placeholders.) */
#define SD_GC_SIG_GUARDS_INIT { \
  { 0x0fd39bcu, 0xb9428f01u, "GC_sig_suspend load: ldr w1, [x24, #0x28c]" }, \
  { 0x0fd39c0u, 0x944b29a0u, "suspend: bl pthread_kill" }, \
  { 0x0fd3c1cu, 0xb9429301u, "GC_sig_thr_restart load: ldr w1, [x24, #0x290]" }, \
  { 0x0fd3c20u, 0x944b2908u, "restart: bl pthread_kill" }, \
  { 0x0fd389cu, 0x9000c000u, "ack sem page (sem_post): adrp x0, #0x27d3000" }, \
  { 0x0fd38a0u, 0x911b4000u, "ack sem (sem_post): add x0, x0, #0x6d0" }, \
  { 0x0fd3b3cu, 0x9000c014u, "ack sem page (sem_wait): adrp x20, #0x27d3000" }, \
  { 0x0fd3b40u, 0x911b4294u, "ack sem (sem_wait): add x20, x20, #0x6d0" }, \
}

#define SD_GC_THREAD_GUARDS_INIT { \
  { 0x0fd3978u, 0x911bc2d6u, "GC_threads base (add)" }, \
  { 0x0fd3988u, 0xf8757adau, "bucket load" }, \
  { 0x0fd3990u, 0xf9400740u, "p->id (pthread_kill arg)" }, \
  { 0x0fd39acu, 0xf9400b48u, "p->stop_info.last_stop_count" }, \
  { 0x0fd39b0u, 0xf94362e9u, "GC_stop_count" }, \
  { 0x0fd39ecu, 0xf940035au, "p->next" }, \
  { 0x0fd39f8u, 0xf10402bfu, "bucket count" }, \
  { 0x0fd2e08u, 0xf9400f95u, "GC_push_all_stacks: lo = p->stop_info.stack_ptr" }, \
  { 0x0fd2e2cu, 0xb40005d5u, "cbz lo -> abort(sp not set)" }, \
}

/* Highest byte the bridge touches; checked against the mapped size at arm. */
#define SD_GC_MAX_OFF  (SD_GC_THREADS_OFF + SD_GC_THREADS_BUCKETS * sizeof(void *))

void sd_gc_arm(uintptr_t il2cpp_base, size_t il2cpp_size);
int  sd_gc_pthread_kill(void *pthread_target, int sig);
void sd_gc_report(void);

/* Stall evidence for the watchdog (lock-free; see sd_gc.c). */
#include <stdint.h>
int sd_gc_stall_info(uint64_t *collector_tid, double *secs, unsigned *round, int *paused);
int sd_gc_event(unsigned back, int *op, uintptr_t *pth, int *pr, double *ago);

#endif /* SD_GC_H */
