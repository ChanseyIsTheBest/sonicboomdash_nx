/* sd_offsets.h -- Sonic Dash 2: Sonic Boom / libunity.so 6000.0.72f1 engine offsets
 *
 * EVERY VALUE HERE WAS DERIVED FROM SD2's OWN BINARY. Nothing is carried over
 * from Sonic Dash 1 except macro names (so sd_patches.c is unchanged) and the
 * method. Guard words were READ FROM SD2's libunity.so at each derived offset.
 *
 * Target:  com.sega.sonicboomandroid 3.24.0, libunity.so BuildID 6689ac689a52c293
 *          Unity 6000.0.72f1 (b731fd3ae857), IL2CPP, metadata v31, arm64-v8a
 *          .text @0x3e9f80, 14,451,304 bytes, 4 PT_LOADs
 *          unity.strip-engine-code=true, so no public symbol file matches this
 *          Build ID; offsets are fingerprinted against the reference below.
 *
 * Reference (the SAME pair SD1 used -- same engine version):
 *          libunity.so / libunity_sym.so BuildID aec5f4ea906b4626bc5230f734e2e2ea2c06ec7f
 *
 * All values are LINK-TIME RVAs into libunity.so:  runtime = libunity_virtbase + RVA.
 *
 * GUARDS. Each site carries its first one or two instruction words as they
 * appear in SD2's binary. sd_patches.c refuses any site whose guard does not
 * match and logs the mismatch. 24 of the 29 guard words are byte-identical to
 * SD1's (same engine, same compiler); the other five encode positions (bl,
 * adrp, and the counter's page offset) and were expected to differ.
 *
 * HOW EACH VALUE WAS ESTABLISHED (tools/derive2.py unless noted):
 *   - whole-body score >= 0.79 with a clear margin, for the plain functions;
 *   - initJni / nativeUnityPlayerSetRunning / JNI_OnLoad: CONFIRMED by the
 *     RegisterNatives table and .dynsym (initJni sig (Landroid/content/Context;I)V);
 *   - sibling pairs separated by reading the binary, not by score (see each);
 *   - FMOD setOutput resolved through its one call site in InitNormal;
 *   - data globals by positional correspondence + reference symbol names.
 */
#ifndef SD_OFFSETS_H
#define SD_OFFSETS_H

/* ------------------------------------------------------------------------
 * Entry points -- libunity's JNI surface
 * ------------------------------------------------------------------------ */
#define SD_OFF_JNI_OnLoad                         0x006c717c   /* == .dynsym JNI_OnLoad */
#define SD_JNI_OnLoad_W0                          0xd10083ffu
#define SD_OFF_initJni                            0x006c5fcc   /* == RegisterNatives "initJni" */
#define SD_initJni_W0                             0xa9be57feu
#define SD_OFF_UnityPlayerLoop                    0x006c52c0
#define SD_UnityPlayerLoop_W0                     0xd10103ffu
#define SD_OFF_nativeUnityPlayerSetRunning        0x006c70d0   /* == RegisterNatives */
#define SD_nativeSetRunning_W0                    0xa9be57feu

/* initJni and nativeUnityPlayerSetRunning share their first words, so these
 * guards are corruption checks only. derive2.py scored the pair 1.0 / 0.905
 * (a TIE by its rule); the RegisterNatives table captured from JNI_OnLoad
 * settles it -- and main.c resolves both from that table at runtime anyway. */

/* ------------------------------------------------------------------------
 * Engine clock -- TimeManager
 * ------------------------------------------------------------------------ */
#define SD_OFF_TimeManager_Update                 0x005005ec   /* score 0.976, margin 0.96 */
#define SD_TM_Update_W0                           0xf940b008u   /* ldr x8, [x0, #0x160] */
#define SD_TM_Update_W1                           0xb9416809u   /* ldr w9, [x0, #0x168] */

/* Field offsets, read off Update's first three instructions IN SD2:
 *      ldr  x8,  [x0, #0x160]     frameCount   u64
 *      ldr  w9,  [x0, #0x168]     aux counter  u32
 *      ldrb w10, [x0, #0x1a8]     paused       u8
 * Identical to SD1 (same engine), and pinned by the W0/W1 guards above. */
#define SD_TM_FIELD_FRAMECOUNT                    0x160   /* u64 */
#define SD_TM_FIELD_AUX                           0x168   /* u32 */
#define SD_TM_FIELD_PAUSE                         0x1a8   /* u8  */

/* The body proper begins after the early-out at +0x24 (cbz w10 / ret /
 * sub sp,sp,#0x90) -- verified in SD2 at 0x00500610. */
#define SD_TM_UPDATE_BODY_ENTRY                   0x24
#define SD_TM_UPDATE_FRAME                        0x90

#define SD_OFF_TimeManager_SetTimeScale           0x00500ed0
#define SD_TM_SetTimeScale_W0                     0xd10303ffu

/* ------------------------------------------------------------------------
 * Frame pacing -- target frame rate, vsync, Swappy
 * ------------------------------------------------------------------------ */
#define SD_OFF_GetVSyncBasedTargetFrameRate       0x004ffe1c   /* score 0.963 */
#define SD_VSFR_W0                                0xfc1e0fe8u   /* str d8, [sp, #-0x20]! */
#define SD_VSFR_W1                                0xa9014ffeu   /* stp x30, x19, [sp, #0x10] */

#define SD_OFF_WaitVSync                          0x006b2be4   /* 24/24 insns identical to ref */
#define SD_WaitVSync_W0                           0xf81d0ffeu

#define SD_OFF_Swappy_UpdateSwapInterval          0x0069eef8   /* score 0.859, margin 0.70 */
#define SD_SwappyUSI_W0                           0xf81d0ffeu
#define SD_OFF_Swappy_GetTargetFrameRate          0x0069ee40
#define SD_SwappyGTFR_W0                          0xfc1e0fe8u

/* Swappy::IsEnabledAndActive() does not resolve (NO_MATCH, as in SD1) --
 * inlined. Not needed: SD2's PlayerSettings has androidUseSwappy = False
 * (read from globalgamemanagers). Do not invent an offset for it. */

/* ------------------------------------------------------------------------
 * Android system probes -- /proc, CPU topology, physical memory
 * ------------------------------------------------------------------------ */
#define SD_OFF_GetBigLittleConfiguration          0x006a59fc   /* score 0.919, margin 0.92 */
#define SD_GBL_W0                                 0xfc190fe8u

#define SD_OFF_GetPhysicalMemoryMB                0x006b6080
#define SD_PHYSMEM_W0                             0xf81f0ffeu
/* Only 6 instructions, so not trusted on score alone. Body identical to the
 * reference (bl GetCachedSystemMemoryInfo; ldr x8,[x0,#8]; lsr x0,x8,#20), and
 * four independently located callers (InitializeDefaultAllocators,
 * BatchDeleteManager ctor, GraphicsCaps::SharedCapsPostInitialize,
 * UnityInitApplication) each call it at the reference's exact offset. */

#define SD_OFF_GetCachedSystemMemoryInfo          0x006decb0
#define SD_OFF_GetCachedProcessMemoryInfo         0x006dee94
#define SD_PROCFS_READER_W0                       0xa9bf4ffeu   /* shared by BOTH */

/* Byte-identical in shape; strict matching finds neither (BSS moved).
 * tools/derive3.py (shape-only) gives exactly two candidates, separated TWO ways:
 *   by the file each one's lazy constructor reads (followed through adr x2):
 *      0x006decb0  slot +0xd40  -> "/proc/meminfo" (MemTotal:/MemFree:)  = System
 *      0x006dee94  slot +0xe40  -> "/proc/self/statm"                    = Process
 *   and by ordering: 0x1e4 apart, lower = System, exactly as in the reference
 *   (0xa80164/+0x640 System, 0xa80348/+0x740 Process). */
#define SD_SYSMEM_SLOT                            0xd40
#define SD_PROCMEM_SLOT                           0xe40

/* What to report (judgement values, unchanged from SD1). */
#define SD_PHYSMEM_MB                             2048
#define SD_SYSMEM_TOTAL_BYTES                     (2048ull << 20)
#define SD_SYSMEM_AVAIL_BYTES                     (1536ull << 20)
#define SD_PROCMEM_RESIDENT_BYTES                 (512ull  << 20)

/* ------------------------------------------------------------------------
 * Choreographer -- Unity 6's frame-pacing path (see PORTING.md s.4)
 * ------------------------------------------------------------------------ */
#define SD_OFF_ChoreographerJava_ctor             0x006b88b0
#define SD_CHOREO_CTOR_W0                         0xd10103ffu
#define SD_OFF_ChoreographerJava_Enable           0x006b8b70
#define SD_CHOREO_ENABLE_W0                       0xd10083ffu
#define SD_OFF_ChoreographerJava_Disable          0x006b8bac
#define SD_OFF_ChoreographerJava_HandleMessage    0x006b8be8
#define SD_CHOREO_HANDLEMSG_W0                    0xd100c3ffu

/* Enable() and Disable() are the same 15 instructions except word 2, the
 * message they post: Enable `mov w8, #1`, Disable `mov w8, #2` -- identical in
 * the reference. That word, not the score (a 1.0/0.933 tie), identifies them;
 * the order also matches the reference (Enable < Disable). Anchors only. */

/* ------------------------------------------------------------------------
 * Audio -- output selection and the AudioManager property probes
 * ------------------------------------------------------------------------ */
#define SD_OFF_GetAndroidAudioOutputType          0x006c25f4   /* score 0.907 */
#define SD_AUDIOOUT_W0                            0xd10203ffu
#define SD_AUDIOOUT_OPENSL                        2   /* force the OpenSL path */

#define SD_OFF_GetNativeOutputSampleRate          0x006c23bc
#define SD_OFF_GetNativeOutputFramesPerBuffer     0x006c2280
#define SD_AUDIOPROP_W0                           0xd10103ffu   /* shared by BOTH */

/* The shared W0 cannot tell the two getters apart. The word at +0x10 is the
 * BL to each one's PROPERTY_* accessor, and it is the guard that proves which
 * is which. (In SD2 the FIRST call, at +0xc, is shared by both -- only +0x10
 * differs.) Separated by reading each accessor's own body:
 *      0x006c23bc -> 0x00df8f08 -> "PROPERTY_OUTPUT_SAMPLE_RATE"
 *      0x006c2280 -> 0x00df8d84 -> "PROPERTY_OUTPUT_FRAMES_PER_BUFFER"
 * derive2.py reported the higher address for BOTH (a 0.974 tie); do not trust
 * its rva on a TIE. */
#define SD_AUDIOPROP_RATE_W4                      0x941cdacfu   /* bl accessor(PROPERTY_OUTPUT_SAMPLE_RATE)       */
#define SD_AUDIOPROP_FRAMES_W4                    0x941cdabdu   /* bl accessor(PROPERTY_OUTPUT_FRAMES_PER_BUFFER) */

/* SAMPLE RATE 48000 / FRAMES 256 -- justified by SD2's OWN AudioManager
 * (globalgamemanagers): m_SampleRate = 0 (defer to the device) and
 * m_DSPBufferSize = 1024. FMOD's OpenSL init fails with error 60 unless
 * rate != 0, frames != 0, frames <= (numbuffers - 1) * bufferlength;
 * 256 clears the >= 1024 bound with margin. Same settings as SD1. */
#define SD_AUDIO_NATIVE_SAMPLE_RATE               48000
#define SD_AUDIO_NATIVE_FRAMES_PER_BUFFER         256

/* ------------------------------------------------------------------------
 * Time / vsync group
 *
 * Unity 6 renamed this area: GetVSyncTime() / GetFrameTimeNanos() (battd_nx,
 * 2020.3) do not exist; their job is AndroidVSync::WaitForLastPresentation-
 * AndGetTimestamp().
 * ------------------------------------------------------------------------ */
#define SD_OFF_TimeManager_SetPause               0x005002d4   /* 3 insns  */
#define SD_OFF_TimeManager_GetTargetFrameTime     0x00500318   /* 39 insns */
#define SD_OFF_TimeManager_EndSyncFrame           0x005003b4
#define SD_OFF_TimeManager_Sync                   0x00500500
#define SD_OFF_GetTimeSinceStartup                0x0053ffd0
#define SD_OFF_EnableFrameTimeTracker             0x006b2d20
#define SD_OFF_AndroidVSync_WaitForLastPresentation  0x00696c60   /* 119/119 insns agree */

/* EnableFrameTimeTracker is the 2020.3 frame-2 Looper deadlock site battd_nx
 * patched to a bare `ret`. Patch it out if boot stalls around frame 2.
 * AndroidVSync::UpdateTimeManager() is one instruction (a tail branch): NO
 * anchor, no value. Reach it through its caller. */

/* ------------------------------------------------------------------------
 * VSync data globals -- by positional correspondence
 *
 * The SD2 and reference bodies of WaitForLastPresentationAndGetTimestamp agree
 * on all 119 mnemonics, so each global access sits at the same index in both;
 * the reference symbol table then NAMES each global:
 *   idx 48 read / 102 write        AndroidVSync::s_LastTimestamp
 *   idx 60 read / 92, 100 write    AndroidVSync::s_LastVsyncCounter
 *   idx 76 address                 g_GfxThreadingMode
 * ------------------------------------------------------------------------ */
#define SD_OFF_AndroidVSync_LastVsyncCounter      0x01290490   /* .bss,  u64 */
#define SD_OFF_AndroidVSync_LastTimestamp         0x01205ae0   /* .data, u64 */
#define SD_OFF_g_GfxThreadingMode                 0x012c4738   /* .bss,  u32 */

/* s_LastVsyncCounter IS NOT THE COUNTER TO TICK: it is "the last target
 * presented" (target = s_LastVsyncCounter + interval, stored back). Ticking it
 * from outside pushes every target further out. Tick the LIVE one below. */

/* ------------------------------------------------------------------------
 * The LIVE vsync counter -- the one the clock thread ticks at 60 Hz
 *
 * WaitVSync(target) at 0x006b2be4, identical to the reference:
 *     pthread_mutex_lock (s_VsyncMonitor      @ 0x012953c0)
 *   loop:
 *     ldr x21, [s_FrameCounter @ 0x01295418]      <- live counter
 *     cmp x21, target ; b.ge done
 *     pthread_cond_wait(s_VsyncMonitor+0x28, s_VsyncMonitor)
 * The pthread calls were confirmed through the PLT relocations, and the names
 * s_VsyncMonitor / s_FrameCounter come from the reference symbol table.
 * counter - mutex = 0x58, exactly as in SD1.
 * ------------------------------------------------------------------------ */
#define SD_OFF_ANDROID_VSYNC_COUNTER              0x01295418   /* .bss, u64 -- TICK THIS */
#define SD_OFF_VSYNC_MUTEX                        0x012953c0   /* bionic pthread_mutex_t slot */
#define SD_OFF_VSYNC_COND                         0x012953e8   /* = mutex + 0x28 */
/* Guards: WaitVSync's own adrp/ldr of the counter. */
#define SD_OFF_WaitVSync_ldr                      0x006b2c08
#define SD_WAITVSYNC_ADRP                         0xf0005f16u   /* adrp x22, #0x1295000     */
#define SD_WAITVSYNC_LDR                          0xf9420ed5u   /* ldr  x21, [x22, #0x418]  */
#define SD_VSYNC_PERIOD_NS                        16666667ull

/* ------------------------------------------------------------------------
 * Choreographer FREE-RUN -- the primary pacing mechanism
 * ChoreographerBase::Get() patched to return NULL: Unity creates neither
 * choreographer and free-runs against the live counter. Guard words are
 * byte-identical to SD1's and colorsheep_nx's (d10303ff / a90a57fe).
 * ------------------------------------------------------------------------ */
#define SD_OFF_ChoreographerBase_Get              0x006b2dc0   /* score 0.869, margin 0.78 */
#define SD_CHOREO_GET_W0                          0xd10303ffu   /* sub sp, sp, #0xc0 */
#define SD_CHOREO_GET_W1                          0xa90a57feu

/* TimeManager::Update's real frame starts here; the detour calls it directly. */
#define SD_TM_BODY_W0                             0xd10243ffu   /* sub sp, sp, #0x90 */

/* GetBigLittleConfiguration returns a 16-BYTE STRUCT in x0:x1 (counts, masks).
 * Zero cores crashes the job scheduler. 3 uniform cores on 0-2. */
#define SD_GBL_X0                                 0x0000000000000003ull  /* big=3, little=0 */
#define SD_GBL_X1                                 0x0000000000000007ull  /* mask 0x7, 0     */

/* ------------------------------------------------------------------------
 * FMOD -- forcing the OpenSL output at the call site FMOD actually reads
 *
 * GetAndroidAudioOutputType -> 2 alone is not sufficient (Colour Sheep); the
 * value FMOD consumes is the argument at the call site in AudioManager::InitNormal:
 *
 *   0x007fd590  bl   GetPlatformOutputOverride    (0x008101c0)
 *   0x007fd594  tbz  w0, #0, ...
 *   0x007fd598  ldr  x0, [x19, #0x178]            ; FMOD::System*
 *   0x007fd59c  ldr  w1, [sp, #0x2c]              ; <-- PATCH SITE
 *   0x007fd5a0  bl   FMOD::System::setOutput      (0x00e7e698)
 *
 * setOutput itself tied FOUR ways (0xe7eb18, 0xe7e854, 0xe7e7ec, 0xe7e698 --
 * FMOD's public wrappers share one shape) and derive2.py ranked 0xe7eb18
 * first. The reference InitNormal calls setOutput exactly once, at +0x88,
 * after GetPlatformOutputOverride; SD2's InitNormal is word-identical there
 * (bar branch targets), so setOutput = the BL target 0xe7e698 -- the
 * fourth candidate, exactly as in SD1.
 *
 * WHY 0x16, read from SD2's GetPlatformOutputOverride (0x008101c0):
 *     cmp w0,#1 -> 0x15 AudioTrack     cmp w0,#2 -> 0x16 OpenSL
 *     cmp w0,#4 -> 0x17                default   -> 0x18 AAudio
 * ------------------------------------------------------------------------ */
#define SD_OFF_FMOD_SETOUTPUT_SITE                0x007fd59c
#define SD_FMOD_SETOUTPUT_FROM                    0xb9402fe1u   /* ldr w1, [sp, #0x2c]  */
#define SD_FMOD_SETOUTPUT_NEXT                    0x941a043eu   /* bl  setOutput        */
#define SD_FMOD_SETOUTPUT_TO                      0x528002c1u   /* mov w1, #0x16        */
#define SD_FMOD_OUTPUTTYPE_OPENSL                 0x16          /* 22 */

/* Diagnostic anchors -- not patched, but where to look when audio fails. */
#define SD_OFF_AudioManager_InitNormal            0x007fd518   /* score 0.926, 513 words */
#define SD_OFF_AudioManager_InitFMOD              0x007fcdbc
#define SD_OFF_GetPlatformOutputOverride          0x008101c0   /* also InitNormal+0x78's BL target */
#define SD_OFF_FMOD_System_setOutput              0x00e7e698
#define SD_OFF_FMOD_OutputOpenSL_init             0x00e8ab0c   /* score 0.958, margin 0.96 */
#define SD_OFF_FMOD_OutputOpenSL_registerLib      0x00e8aa18
#define SD_OFF_FMOD_OpenSL_DriverConfigCheck      0x00e8ac40   /* init+0x134, as SD1 */

/* If the log says "FMOD failed to initialize the output device (60)":
 *   - no dlopen("libOpenSLES.so") logged  -> registerLib failed, or the
 *     setOutput patch above did not apply
 *   - dlopen + slCreateEngine logged      -> the driver-config check at
 *     init+0x134 (ldr x9,[x29,#0x10]; cbz ...), whose failure path at
 *     init+0x18c returns 60 via init+0x84 (mov w0,#0x3c) -- one of the two
 *     AudioManager getters returned 0.
 * Error 33 (0x21, init+0x12c) means an OpenSL object call failed -- look in
 * opensles.c. */

#endif /* SD_OFFSETS_H */
