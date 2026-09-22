/* config.h -- Sonic Dash 2: Sonic Boom, Switch wrapper configuration.
 *
 * Forked from the laytonbmr_nx / vln_nx SoLoader ports (MIT). The loader-tuning
 * constants (heap split, mmap arena, overcommit window) are inherited unchanged;
 * they are engine-generation properties, not game properties. The game-identity
 * and user-config parts below are Sonic-Dash-2-specific.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* Newlib heap for the engine/libc++/il2cpp managed heaps; the rest -> .so loader. */
#define MEMORY_MB 768

/* mmap arena. Unity reserves aligned pools by over-mmapping then trimming head/tail;
 * we back anon mmaps from an aligned arena with a per-page bitmap so sub-range munmap
 * frees only trimmed pages. ALIGN must match Unity's region granularity. */
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)
#define MMAP_ARENA_RESERVE  ((size_t)1792 * 1024 * 1024)  /* heap-backed cap (28x64MB) */

/* Stack-region overcommit arena (libc_shim.c). */
#define OC_WINDOW_BYTES     ((size_t)1536 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 384 * 1024 * 1024)
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

/* --- inherited SoLoader leftovers (unused by Unity, kept for base parity) --- */
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"
#define MAIN_MVGL    "main.10007.android.mvgl"

/* --- Sonic Dash 2: Sonic Boom game identity (JNI Context shim) ---
 * Unity 6000.0.72f1 (b731fd3ae857) / IL2CPP / arm64-v8a, metadata v31.
 * Every value below was read from the game's own files (AndroidManifest.xml,
 * assets/bin/Data/unity_app_guid).
 *
 * ASSET LAYOUT (not SD1's). SD2 is an App Bundle of TWO pieces: the base APK,
 * which carries ALL game data, and the arm64 split, which carries the .so
 * files. There is no asset pack, no OBB and no Addressables:
 *     assets/bin/Data/            classic player data: globalgamemanagers,
 *                                 level0..level124, sharedassets*, ~3,500
 *                                 hash-named serialized files, Resources/,
 *                                 Managed/Metadata/global-metadata.dat
 *     sharedassets{0,4,5,33}.assets ship as 1 MB .splitN chunks; they are
 *                                 joined at first boot (nx_splitjoin.c).
 * ~3,800 files in ONE directory are slow to open on a Switch SD card, so the
 * whole assets/ tree is packed into assets.nxpack at first boot (asset_pack.c,
 * from fruitninjaclassic_nx) and served from there. */
#define SD_PACKAGE       "com.sega.sonicboomandroid"
#define SD_VERSION_NAME  "3.24.0"
#define SD_VERSION_CODE  1812621967
#define SD_APP_GUID      "9d7524d2-4e91-4a93-98bc-284dddba0a64"  /* unity_app_guid */

#define CONFIG_NAME "config.txt"
#define LOG_NAME    (sd_log_path())          /* <game folder>/debug.log */

/* Game data root == the .nro's own folder, WHICHEVER folder that is: nro +
 * libs + assets live together, and sd_home.c finds them at run time (argv[0],
 * else the current directory, else sdmc:/switch/GAME_FOLDER_DEFAULT). GAME_HOME
 * is therefore a function call, never a string literal to paste onto. */
#define GAME_FOLDER_DEFAULT "sonicboomdash"
const char *sd_home(void);          /* sd_home.c */
const char *sd_home_bare(void);
const char *sd_log_path(void);
#define GAME_HOME   (sd_home())

/* flip to 1 (and rebuild) for on-hardware file logging (debug.log). 0 is the
 * release build: no debug.log at all, and the diagnostic tracers (UI taps, JNI
 * calls -- sd_config.c) stay off, since there is nowhere for them to write. */
#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

/* =======================================================================
 * Sonic Dash 2 additions
 * ======================================================================= */

/* Force GLES by refusing libvulkan.so at dlopen. SD2 is effectively a GLES3
 * build: all 96 Shader objects carry GLES3 programs (2,316 x "#version 300 es",
 * 39 x "310 es", read from the decompressed blobs) and only 6 also have Vulkan.
 * Leave this on: mesa/nouveau is OpenGL only. */
#define SD_REFUSE_VULKAN            1

/* 60 fps override. OFF by default -- see the warning in sd_patches.c and
 * PORTING.md section 6. An endless runner is exactly the genre where a
 * per-frame integration bug turns 60 fps into double speed. */
#define SD_FORCE_60FPS              0

/* Resolution is NOT set here: config.txt's `resolution` (sd_config.c), 720p by
 * default and the same in docked and handheld. rotation 0 (default) renders
 * landscape 16:9 straight to the window, as Sonic Dash 1 did. SD2's manifest
 * (screenOrientation=1) and PlayerSettings declare portrait only, yet like SD1
 * it is played landscape here. Portrait stays available: rotation 1/2 turn it
 * onto the panel, rotation 3 draws it upright and centred. See sd_tate.c. */

/* COUNTRY, pinned. Every path that asks the "device" for its country answers
 * this, so compliance rules, region-gated content, promos and the server's user
 * country all agree. It is independent of LANGUAGE, which still follows the
 * Switch's system setting (locale strings become "<language>_AU").
 *
 * Why it matters: HLUnityCore.Unity_GetISO2CountryCode used to answer "", which
 * Hardlight's ComplianceUtility.FindCountry (String.IsNullOrWhiteSpace) turned
 * into the country "default" -- recorded in LegalPluginData.json -- where a phone
 * records its real country. The lookup is case-insensitive (String.Equals with
 * OrdinalIgnoreCase), so "AU" matches the list's "au".
 * The game's own regulation table: {"id":"au","age":15,"type":"coppa"}. */
#define SD_COUNTRY_ISO2  "AU"
#define SD_COUNTRY_ISO3  "AUS"

/* JNI LOGGING -- master switch. 1 = everything:
 *   - util.c's log_is_noisy() stops dropping "[jni]", "JNI:", "dlopen", "dlsym"
 *   - [jnim]   every distinct Java method, once        (sd_jni.c)
 *   - [jniret] what we ANSWERED, first 3 calls a method (sd_jni.c)
 *   - [jnicls] every class, once                        (always on)
 * Flood-guarded: UNIMPL slots and pool exhaustion log once, dlsym is capped.
 * Off in a release build: the per-call answer bookkeeping then compiles away.
 * Follows DEBUG_LOG -- there is nothing to write to without the log. */
#define SD_JNI_LOG  DEBUG_LOG

/* JNI answers found wrong (from the logs), one switch each, so a change can be
 * attributed. Each is written and builds either way:
 *   SD_JNI_FIX_CLASS    java.lang.Class: getName/getSimpleName/isArray/... on
 *                       class objects (was routed like a method of the class it
 *                       describes -- BillingResult's class answered with the
 *                       billing debug message), and getClass() of strings and
 *                       boxed values. Unity's Unbox runs getClass().getName()
 *                       on every Java value handed to C#.            ON
 *   SD_JNI_FIX_STRINGS  String.equals & co. compare contents (equals fell
 *                       through to false); Environment.MEDIA_MOUNTED. NOTE:
 *                       Unity then sees external storage as mounted and may move
 *                       persistentDataPath -- where the save lives -- to
 *                       getExternalFilesDir. Check that path first.   OFF
 *   SD_JNI_FIX_SCANNER  java.util.Scanner: Unity reads bin/Data/boot.config
 *                       through it and gets "" (gfx-threading-mode etc. then take
 *                       engine defaults). Left as is by choice.       OFF */
#define SD_JNI_FIX_CLASS   1
#define SD_JNI_FIX_STRINGS 0
#define SD_JNI_FIX_SCANNER 0

/* ANDROID VERSION reported to the game AND the engine (Build.VERSION.SDK_INT /
 * RELEASE). 32 = Android 12L: the newest level WITHOUT Android 13's runtime
 * notification permission. At 33+ the game's notification setup
 * (Hardlight.AndroidNotificationsNativeBridge.RequestPermissions) asks through
 * Unity's PermissionCallbacks proxy, and this port cannot yet call a Java proxy
 * back -- the request would stay pending forever. Before the Class.forName fix
 * the engine saw 0 and C# saw 33; both now see this one value. */
/* From SD2's own AndroidManifest.xml. */
#define SD_APK_MIN_SDK      23
#define SD_APK_TARGET_SDK   36

#define SD_ANDROID_API      32
#define SD_ANDROID_RELEASE  "12"

/* RAM read-ahead cache for big read-only asset files (libc_shim.c): 8 slots x
 * a 1 MB window, turning Unity's thousands of tiny per-field reads into one
 * large SD read. Disabled while the stop-the-world freeze is investigated
 * (user request). Audited first: it is small (<= 8 MB) and correct -- close()
 * detaches the slot, so a reused fd is never served stale bytes. Expect
 * slower loading with it off; set 1 to restore. */
#define SD_RA_CACHE  0

/* Deliver Google Play Billing's onBillingSetupFinished(BillingUnavailable) to
 * Unity IAP (sd_jni.c). It runs C# on the proxy drain thread -- the first C#
 * to run on a thread this port created -- just before the first stop-the-world
 * freeze. 1 = deliver (default). Set 0 to bisect: if the freeze goes away,
 * C# on the drain thread is implicated. */
#define SD_BILLING_CALLBACK  1

/* (No render-scale switch: Sonic Dash 1 rendered at 75% through
 * AppFlow.SetResolutionScale -> Screen.SetResolution. SD2 has no AppFlow, its
 * Screen class has no SetResolution at all -- stripped, nothing calls it -- and
 * PlayerSettings.resolutionScalingMode is 0. It always renders at full size.) */

/* Analytics off: the game's own opt-out path (Hardlight.Analytics.
 * AnalyticsSystem.CanAnalyticsEventsBeCollected -> false). SD2 carries the
 * same Hardlight class and method as SD1, where offline every event failed
 * and was appended to failed_events.log forever. */
#define SD_ANALYTICS_OFF    1

/* Rotated presentation: the portrait image is blitted 1:1 onto the window in
 * every mode, so NEAREST sampling is exact. 1 = LINEAR (softer). */
#define SD_TATE_LINEAR  0

/* (No save editor: Sonic Dash 1's edited Hardlight's PropertyStore save,
 * signed with the salt "Artifical Key". SD2 has neither the class nor the
 * salt -- its save format is different and not yet established, so that
 * editor was removed rather than shipped against the wrong format.) */

/* ASSET PACK (asset_pack.c, from fruitninjaclassic_nx). On the first boot the
 * whole assets/ tree -- ~3,800 files, ~225 MB -- is copied into ONE file,
 * assets.nxpack (+ its index assets.nxidx), verified by reading it back, and
 * mounted. Every later open/stat/read of a game asset is a binary search in
 * RAM plus a read from that one file, instead of a lookup in a ~3,800-entry
 * SD directory. Needs ~250 MB free on the SD card for the first boot. */
#define SD_ASSET_PACK         1

/* Delete the loose assets/ tree once the pack is built, verified and mounted
 * (fruitninjaclassic_nx's behaviour; requested for this port). Frees ~225 MB.
 * An empty directory skeleton is recreated at every boot for Unity's runtime
 * writes. To rebuild the pack (e.g. after a game update), delete
 * assets.nxpack and assets.nxidx and copy assets/ back. */
#define SD_PACK_DELETE_LOOSE  1

/* nx_splitjoin.c: keep the .splitN parts after joining. The whole loose tree
 * is deleted after packing anyway, so this only matters with the pack off. */
#define JOIN_DELETE_PARTS     0

#endif /* __CONFIG_H__ -- everything above is inside the guard */
