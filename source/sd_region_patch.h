/* sd_region_patch.h -- Unity allocator region granularity, 256 MB -> 64 MB.
 * DERIVED FROM SD2's libunity.so (BuildID 6689ac689a52c293). 16 sites.
 * Generated from scan_granularity.py's classifier over SD2's ten allocator
 * functions (located by derive2.py, all OK, scores 0.95-1.0). The from/to
 * words, roles and order are identical to SD1's table; only addresses moved.
 *
 * WHY: Unity reserves allocator blocks at 256 MB granularity. On a 4 GB Switch
 * the wrapper's mmap arena cannot satisfy those reservations and the game
 * deadlocks at the first frame. 64 MB blocks fit (~4x more).
 *
 * WHY A NAIVE "DIVIDE EVERY 28 BY 4" REWRITE IS WRONG
 * The block metadata is a TWO-LEVEL table:
 *     region = ptr >> 28        <- granularity, must become >> 26
 *     L1     = region >> 12     <- the split. FIXED.
 *     L2     = region & 0xFFF   <- a FIXED 4096-entry table
 * The L2 index WIDTH must not change. Widening it from 12 to 14 bits to
 * "match" the new granularity indexes 4x past the end of a fixed allocation.
 * Keeping width 12 just means each L2 table covers 4096 x 64MB = 256 GB
 * instead of 1 TB, which is irrelevant on a 39-bit address space.
 *
 * And both levels must move TOGETHER. Patch only the granularity and L2 drops
 * to bits 26..37 while a fused L1 still starts at bit 40 -- bits 38-39 become
 * unmapped and addresses 256 GB apart COLLIDE. That is silent, ASLR-dependent
 * heap corruption, not a clean failure.
 *
 * ROLES
 *   shift       ptr >> 28            -> ptr >> 26        (lsb -2)
 *   index       ubfx lsb=28,width=W  -> lsb=26,width=W   (WIDTH UNCHANGED)
 *   basemask    and #0xfffffff0000000 -> #0xfffffffc000000
 *   size/sizek  0x10000000            -> 0x4000000       (the region size)
 *   scale       Rn, lsl #28          -> lsl #26          (region -> bytes)
 *   l1step      -(1<<36)             -> -(1<<38)         (step one L1 table)
 *   fusedL1     ptr >> 40            -> ptr >> 38        (L1 in one insn)
 * The L1/L2 SPLIT itself (ubfx w8,w8,#0xc,#0x10 at 0x0042fdc0, and the
 * matching asr #0xc) is deliberately NOT in this table. It is correct as-is.
 *
 * COMPLETENESS -- this is the part that bites.
 * Colour Sheep carried an 18-site table from another port; it got past
 * MemoryManager and then NULL-derefed in a block-table lookup, because a newer
 * codegen emitted a site the table did not know about. So the set here was
 * established by SWEEPING THE WHOLE 14 MB .text (re-run on SD2) for each distinctive encoding
 * rather than by trusting a function list:
 *
 *   region base mask 0xfffffff0000000 : 1 occurrence in .text, in this table
 *   l1step constant  -(1<<36)         : 1 occurrence in .text, in this table
 *   fused two-level lookup (lsr#40    : 2 occurrences in .text, BOTH in table
 *     followed by ubfx #28,#12)
 *   two-instruction L1 (lsr#28 then   : 1 occurrence in .text, in this table
 *     ubfx #0xc,#0x10)
 *
 * That sweep is what found site 15/16. MemoryManager::GetAllocatorContainingPtr
 * performs the SAME two-level lookup against a DIFFERENT table (+0x2190 rather
 * than +0x60) and is not part of VirtualAllocator, so a function-list-driven
 * derivation misses it entirely -- and missing it means that one lookup
 * disagrees with every other about which region a pointer belongs to.
 *
 * NOTE: bare `lsr #28` (26x) and `lsr #40` (18x) recur across SD2's .text. Those are ordinary shifts in unrelated code and must NOT be
 * patched. Only the sites below, reached through the allocator, are region
 * arithmetic. This is why the sweep keys on the DISTINCTIVE encodings and then
 * confirms structurally, rather than rewriting every shift-by-28 it finds.
 *
 * APPLICATION IS ALL-OR-NOTHING: nx_patch_unity_regions() verifies every
 * `from` word, logs each mismatch, and patches NOTHING if any site disagrees.
 * A partial patch would mix 256MB and 64MB paths, which is worse than stock.
 */
#ifndef SD_REGION_PATCH_H
#define SD_REGION_PATCH_H

#include <stdint.h>

typedef struct { uint32_t off; uint32_t from; uint32_t to; } SdRegionPatch;

static const SdRegionPatch SD_REGION_PATCH[] = {
  /* VirtualAllocator::MarkMemoryBlocks */
  { 0x042f9dcu, 0xd35cdc33u, 0xd35ad433u },  /* index     ubfx x19, x1, #0x1c, #0x1c */
  { 0x042f9e4u, 0xd35cfd15u, 0xd35afd15u },  /* shift     lsr x21, x8, #0x1c */
  /* VirtualAllocator::ReserveMemoryBlock */
  { 0x042fa88u, 0x52a20008u, 0x52a08008u },  /* size      mov w8, #0x10000000 */
  /* VirtualAllocator::GetMemoryBlockFromPointer */
  { 0x042fdbcu, 0xd35cfc28u, 0xd35afc28u },  /* shift     lsr x8, x1, #0x1c */
  { 0x042fdccu, 0x92646c28u, 0x92667428u },  /* basemask  and x8, x1, #0xfffffff0000000 */
  { 0x042fdd4u, 0xd35c9c2au, 0xd35a942au },  /* index     ubfx x10, x1, #0x1c, #0xc */
  { 0x042fde8u, 0xd35cdc29u, 0xd35ad429u },  /* index     ubfx x9, x1, #0x1c, #0x1c */
  { 0x042fdecu, 0xb25c6febu, 0xb25a67ebu },  /* l1step    mov x11, #-0x1000000000 */
  { 0x042fdf0u, 0xf2a2000bu, 0xf2a0800bu },  /* sizek     movk x11, #0x1000, lsl #16 */
  { 0x042fe34u, 0xcb0a7108u, 0xcb0a6908u },  /* scale     sub x8, x8, x10, lsl #28 */
  /* VirtualAllocator::GetBlockInfoFromPointer */
  { 0x042fe48u, 0xd368fc28u, 0xd366fc28u },  /* fusedL1   lsr x8, x1, #0x28 */
  { 0x042fe58u, 0xd35c9c29u, 0xd35a9429u },  /* index     ubfx x9, x1, #0x1c, #0xc */
  /* BucketAllocator::BucketAllocator */
  { 0x042aac8u, 0x52a20009u, 0x52a08009u },  /* size      mov w9, #0x10000000 */
  /* DynamicHeapAllocator::DynamicHeapAllocator */
  { 0x042d520u, 0x52a20009u, 0x52a08009u },  /* size      mov w9, #0x10000000 */
  /* MemoryManager::GetAllocatorContainingPtr */
  { 0x04317b8u, 0xd368fc28u, 0xd366fc28u },  /* fusedL1   lsr x8, x1, #0x28 */
  { 0x04317d0u, 0xd35c9e89u, 0xd35a9689u },  /* index     ubfx x9, x20, #0x1c, #0xc */
};
#define SD_REGION_PATCH_N  16


#endif /* SD_REGION_PATCH_H */
