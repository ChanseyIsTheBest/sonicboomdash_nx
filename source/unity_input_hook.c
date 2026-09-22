/* unity_input_hook.c -- feed Switch touch straight into UnityEngine.Input (Sonic Dash 2).
 *
 * Unity 6's nativeInjectEvent path is a dead end here: it ACCEPTS our fake MotionEvent
 * (inject_ret=1) but never reads its coordinates, so Input.touchCount / GetTouch /
 * mousePosition stay empty and the game's UI never sees a tap. Instead we bypass the JNI
 * event system entirely and patch the game's own il2cpp Input methods to return OUR touch
 * state directly. The game reads these every frame, so it sees the Switch touchscreen.
 *
 * RVAs come from sd_il2cpp_offsets.h, GENERATED from THIS build's dump.cs by
 * tools/derive_il2cpp_hooks.py (the inherited table held Colour Sheep's RVAs, which
 * lie past the end of this game's libil2cpp.so). runtime = il2cpp load_virtbase + RVA.
 * UnityEngine.Touch was checked against this build's dump.cs: 14 fields, 0x44 bytes,
 * offsets identical to NxTouch below -- and _Static_asserts now hold that in place.
 *
 * (The reference's LanguageParam.getCurrentLanguage hook is REMOVED: that is a Layton
 * game-specific class.)
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "config.h"   /* SD_ANALYTICS_OFF -- without it the #if below silently
                       * evaluated to 0 and the hook was never built (SD1 AUDIT
                       * finding 62; the audit now runs -Wundef) */
#include "unity_input_hook.h"
#include "sd_tilt.h"      /* Input.acceleration / Input.gyro.enabled (the Enerbeam) */

int   debugPrintf(char *fmt, ...);
extern int screen_width, screen_height;
int so_patch_code(void *dst, const void *src, unsigned long len);   /* so_util.c */

/* ---- touch state, written by android_native_feed_hid every frame ----------
 * Multi-touch: g_hook_touch[0..g_hook_count) are the fingers Unity will see this frame
 * (Color Sheep needs several at once -- you hold multiple colour pads together). The
 * mouse-emulation globals mirror the PRIMARY finger so the GetMouseButton* hooks and any
 * mouse-driven UI keep working exactly as before. */
typedef struct { int id; float x, y; int phase; float dx, dy; } HookTouch;  /* phase: 0 Began 1 Moved 2 Stationary 3 Ended */
static HookTouch g_hook_touch[NX_MAX_TOUCH];

int   g_hook_count = 0;                 /* Input.touchCount (0..NX_MAX_TOUCH)            */
int   g_hook_btn   = 0;                 /* Input.GetMouseButton(0)  (primary finger)     */
int   g_hook_btn_down = 0, g_hook_btn_up = 0;  /* GetMouseButtonDown/Up(0), 1-frame edges */
int   g_hook_phase = 3;                 /* primary finger's TouchPhase                   */
float g_hook_x = 0.0f, g_hook_y = 0.0f; /* Unity screen space (bottom-left origin, px)   */

/* ---- Unity value types (AArch64 return conventions matter) ---------------- */
typedef struct { float x, y; }    NxV2;
typedef struct { float x, y, z; } NxV3;                 /* HFA -> s0,s1,s2      */
typedef struct {                                        /* UnityEngine.Touch, 0x44 bytes */
  int32_t m_FingerId;       NxV2 m_Position;   NxV2 m_RawPosition; NxV2 m_PositionDelta;
  float   m_TimeDelta;      int32_t m_TapCount; int32_t m_Phase;   int32_t m_Type;
  float   m_Pressure;       float m_maxPressure; float m_Radius;   float m_RadiusVariance;
  float   m_AltitudeAngle;  float m_AzimuthAngle;
} NxTouch;                                               /* > 16 bytes -> sret (x8) */
/* UnityEngine.Touch in this build's dump.cs (6000.0.72f1). GetTouch returns this BY
 * VALUE, so a drifted layout would hand the game wrong positions or phases with no
 * crash to point at -- broken swipes. Fail the build instead. */
_Static_assert(sizeof(NxTouch) == 0x44,                      "UnityEngine.Touch is 0x44 bytes");
_Static_assert(__builtin_offsetof(NxTouch, m_Position) == 0x04, "Touch.m_Position @0x04");
_Static_assert(__builtin_offsetof(NxTouch, m_TapCount) == 0x20, "Touch.m_TapCount @0x20");
_Static_assert(__builtin_offsetof(NxTouch, m_Phase)    == 0x24, "Touch.m_Phase @0x24");
_Static_assert(__builtin_offsetof(NxTouch, m_Pressure) == 0x2C, "Touch.m_Pressure @0x2C");
_Static_assert(__builtin_offsetof(NxTouch, m_AzimuthAngle) == 0x40, "Touch.m_AzimuthAngle @0x40");

/* ---- the hooks: il2cpp calling convention = (real args..., MethodInfo*) ---- */
/* ---- touch-path instrumentation -------------------------------------------
 * Every link of the chain logs, rate-limited, so a single debug.log shows where
 * touch stops:  HID sees a finger  ->  hook state  ->  the game polls
 * Input.touchCount  ->  the game reads Input.GetTouch.  Added after a boot where
 * no UI responded and the log could not say which link had failed. */
static unsigned s_tc_calls, s_tc_nonzero, s_gt_logged, s_hid_logged;

static void touch_log_hid(const char *what, int id, float x, float y) {
  /* Every tap, up to a generous cap: the first cap (12 events) ended before the
   * character-select screen, so the taps that mattered were never recorded. */
  if (s_hid_logged < 4000) {
    if (++s_hid_logged == 4000) debugPrintf("[touch] (tap log cap reached; later taps not logged)\n");
    debugPrintf("[touch] HID finger %d %s at (%.0f,%.0f)%s\n", id, what, x, y,
                s_tc_calls ? "" : "  -- but the game has NOT polled touchCount yet");
  }
}

static int32_t hk_touchCount(void *mi){
  (void)mi;
  s_tc_calls++;
  if (g_hook_count > 0) s_tc_nonzero++;
  if (s_tc_calls == 1)
    debugPrintf("[touch] game is polling Input.touchCount (first call)\n");
  else if ((s_tc_calls % 1200) == 0)   /* ~4 calls a frame -> about every 5 s */
    debugPrintf("[touch] touchCount polled %u times, %u of them with touches\n",
                s_tc_calls, s_tc_nonzero);
  return g_hook_count;
}

/* The gate outside our other hooks. The game's StandaloneInputModuleNoMouse
 * reports itself supported only if  m_ForceModuleActive || Input.mousePresent ||
 * Input.touchSupported,  and the EventSystem never runs an unsupported module --
 * so if libunity's native layer said "no touchscreen, no mouse", no touch could
 * reach any UI however well the other hooks worked. The Switch has a
 * touchscreen, so this is the truthful answer, not a workaround. */
static uint8_t hk_touchSupported(void *mi){
  (void)mi;
  static int logged;
  if (!logged) { logged = 1; debugPrintf("[touch] Input.touchSupported -> true (input module can activate)\n"); }
  return 1;
}
static NxV3    hk_mousePosition(void *mi){ (void)mi; NxV3 v = { g_hook_x, g_hook_y, 0.0f }; return v; }
static uint8_t hk_getMouseButton(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn : 0; }
static uint8_t hk_getMouseButtonDown(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_down : 0; }
static uint8_t hk_getMouseButtonUp(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_up : 0; }
static NxTouch hk_getTouch(int32_t index, void *mi){
  (void)mi; NxTouch t; memset(&t, 0, sizeof t);
  if (index >= 0 && index < g_hook_count) {
    const HookTouch *h = &g_hook_touch[index];
    t.m_FingerId   = h->id;                     /* stable per finger while it is down */
    t.m_Position.x = h->x; t.m_Position.y = h->y;
    t.m_RawPosition = t.m_Position;
    t.m_Phase = h->phase; t.m_TapCount = 1;
    t.m_Pressure = 1.0f; t.m_maxPressure = 1.0f;
    /* Unity's own UI derives drag deltas from positions, but custom widgets --
     * scroll pickers especially -- often read touch.deltaPosition directly. It
     * was always zero here, which lets taps work while every drag goes nowhere. */
    t.m_PositionDelta.x = h->dx; t.m_PositionDelta.y = h->dy;
    t.m_TimeDelta = 1.0f / 60.0f;
    if (s_gt_logged < 8) {
      s_gt_logged++;
      debugPrintf("[touch] game read GetTouch(%d): id=%d phase=%d pos=(%.0f,%.0f) delta=(%.1f,%.1f)\n",
                  (int)index, h->id, h->phase, h->x, h->y, h->dx, h->dy);
    }
  }
  return t;
}

/* ---- Input.touches: the Touch[] property ------------------------------------
 * FOUND ON HARDWARE -- taps worked, swipes never registered. SD2's swipe
 * detector, SimpleGestureMonitor.ReadRawInput, reads Input.touches. IL2CPP
 * compiled that getter with the engine calls INLINED: it resolves the native
 * icalls "UnityEngine.Input::get_touchCount()" and "::GetTouch_Injected(...)"
 * and calls them directly, never passing through the managed touchCount and
 * GetTouch this file hooks. So it returned Unity's NATIVE touch state -- which
 * this port never fills -- an empty array while Input.touchCount said 1. The UI
 * worked because the EventSystem calls GetTouch(i). A scan of libil2cpp finds
 * those two icall names referenced ONLY by get_touchCount, GetTouch and
 * get_touches, so this closes the last path around the hooks.
 *
 * Built from the same snapshot as hk_getTouch: a managed Touch[] allocated
 * through the exported IL2CPP API (the Touch class looked up by name once, its
 * element size checked against our 0x44-byte layout), elements copied in.
 * Il2CppArray: klass, monitor, bounds, max_length, then the data at +0x20. */
static void *(*p_array_new)(void *elem_class, uintptr_t n);
static void *(*p_domain_get)(void);
static void **(*p_domain_get_assemblies)(const void *domain, size_t *n);
static const void *(*p_assembly_get_image)(const void *assembly);
static void *(*p_class_from_name)(const void *image, const char *ns, const char *name);
static int32_t (*p_array_element_size)(const void *klass);

void nx_input_set_il2cpp_api(void *array_new, void *domain_get, void *domain_get_assemblies,
                             void *assembly_get_image, void *class_from_name, void *array_element_size) {
  p_array_new             = (void *(*)(void *, uintptr_t))array_new;
  p_domain_get            = (void *(*)(void))domain_get;
  p_domain_get_assemblies = (void **(*)(const void *, size_t *))domain_get_assemblies;
  p_assembly_get_image    = (const void *(*)(const void *))assembly_get_image;
  p_class_from_name       = (void *(*)(const void *, const char *, const char *))class_from_name;
  p_array_element_size    = (int32_t (*)(const void *))array_element_size;
}
int nx_input_il2cpp_api_ok(void) {
  return p_array_new && p_domain_get && p_domain_get_assemblies && p_assembly_get_image &&
         p_class_from_name && p_array_element_size;
}

/* UnityEngine.Touch's class, found by name in whichever assembly holds it
 * (UnityEngine.InputLegacyModule here). -1 = looked and failed: do not retry
 * every frame, and say so once. */
static void *touch_class(void) {
  static void *cls; static int state;          /* 0 unknown, 1 ok, -1 failed */
  if (state) return state > 0 ? cls : NULL;
  state = -1;
  if (!nx_input_il2cpp_api_ok()) { debugPrintf("[touch] Input.touches: IL2CPP API not resolved\n"); return NULL; }
  size_t n = 0;
  void **asms = p_domain_get_assemblies(p_domain_get(), &n);
  for (size_t i = 0; asms && i < n && !cls; i++) {
    const void *img = p_assembly_get_image(asms[i]);
    if (img) cls = p_class_from_name(img, "UnityEngine", "Touch");
  }
  if (!cls) { debugPrintf("[touch] Input.touches: class UnityEngine.Touch NOT FOUND in %zu assemblies\n", n); return NULL; }
  const int32_t esz = p_array_element_size(cls);
  if (esz != (int32_t)sizeof(NxTouch)) {
    debugPrintf("[touch] Input.touches: Touch[] element is %d bytes, ours is %zu -- REFUSED\n",
                (int)esz, sizeof(NxTouch));
    cls = NULL; return NULL;
  }
  state = 1;
  debugPrintf("[touch] Input.touches: UnityEngine.Touch class %p, element %d bytes\n", cls, (int)esz);
  return cls;
}

static void *hk_getTouches(void *mi) {
  (void)mi;
  const int n = g_hook_count;                  /* one snapshot: length and elements agree */
  void *cls = touch_class();
  if (!cls) return NULL;
  uint8_t *arr = (uint8_t *)p_array_new(cls, (uintptr_t)n);
  if (!arr) { debugPrintf("[touch] Input.touches: il2cpp_array_new(%d) failed\n", n); return NULL; }
  for (int i = 0; i < n; i++) {
    NxTouch t = hk_getTouch(i, NULL);
    memcpy(arr + 0x20 + (size_t)i * sizeof(NxTouch), &t, sizeof t);
  }
  static unsigned calls, nonzero;
  calls++; if (n) nonzero++;
  s_tc_calls++;                                /* managed code is polling input: same signal as touchCount */
  if (calls == 1) debugPrintf("[touch] game is reading Input.touches (first call)\n");
  if (n && nonzero <= 3)
    debugPrintf("[touch] Input.touches -> %d touch(es), [0] phase=%d pos=(%.0f,%.0f) delta=(%.1f,%.1f)\n",
                n, g_hook_touch[0].phase, g_hook_touch[0].x, g_hook_touch[0].y,
                g_hook_touch[0].dx, g_hook_touch[0].dy);
  return arr;
}

/* ---- tilt (sd_tilt.c) ---------------------------------------------------------
 * Input.acceleration is HFA Vector3 -> s0..s2. Gyroscope.enabled is an instance
 * property: x0 = the Gyroscope, w1 = the value. Only the game's beam controller
 * sets it (and a debug menu); the native gyro is never touched -- there is none. */
static NxV3 hk_acceleration(void *mi) {
  (void)mi; float v[3]; sd_tilt_vector(v);
  NxV3 r = { v[0], v[1], v[2] }; return r;
}
static void    hk_gyro_set_enabled(void *self, uint8_t on, void *mi) { (void)self; (void)mi; sd_tilt_set_beam(on != 0); }
static uint8_t hk_gyro_get_enabled(void *self, void *mi) { (void)self; (void)mi; return (uint8_t)sd_tilt_beam(); }
/* SystemInfo.supportsGyroscope: the Switch has a gyroscope (and accelerometer),
 * and SimpleGestureMonitor.RequestGyroEnabled does nothing without this -- the
 * Enerbeam never "started" for the tilt layer. Truthful, like touchSupported. */
static uint8_t hk_supports_gyro(void *mi) {
  (void)mi;
  static int logged;
  if (!logged) { logged = 1; debugPrintf("[tilt] SystemInfo.supportsGyroscope -> true (the game may enable the gyro)\n"); }
  return 1;
}

/* Overwrite a method entry with an absolute long jump to `target`:
 *   ldr x16, #8 ; br x16 ; .quad target      (16 bytes) */
static void patch_jump(uintptr_t site, void *target) {
  uint32_t code[4];
  code[0] = 0x58000050u;                 /* ldr x16, #8  (load target from site+8) */
  code[1] = 0xd61f0200u;                 /* br  x16                                */
  memcpy(&code[2], &target, sizeof target);
  so_patch_code((void *)site, code, sizeof code);
}

/* ---- guarded hooks ------------------------------------------------------
 * The RVAs come from sd_il2cpp_offsets.h, GENERATED from this build's dump.cs
 * by tools/derive_il2cpp_hooks.py, which verifies the dump matches this
 * libil2cpp.so, bounds-checks every target into executable code, rejects
 * identical-code folding, and requires 16 bytes of room for the jump stub.
 *
 * THIS REPLACES A DANGEROUS INHERITED TABLE. It carried Colour Sheep's RVAs
 * (0x2A2D83C ...) with no guard. Sonic Dash 1's libil2cpp mapped only 0x2615770
 * bytes, so those addresses were past the end of the module: every boot would
 * have written jump stubs into whatever memory followed it.
 *
 * Each hook re-checks its first two instruction words at runtime, and each set
 * is ALL-OR-NOTHING: nothing is written unless every required target verifies. */
#include "sd_il2cpp_offsets.h"

/* `absent` comes from the generator: 1 only when the target's class has NO
 * method of that name in this build (stripped, so nothing can call it). A
 * required target that is merely MISSING (rva 0 without that proof) still
 * refuses the whole set -- the all-or-nothing rule of AUDIT finding 4. */
typedef struct { const char *what; uint32_t rva, w0, w1; void *fn; int required; int absent; } SdHook;
#define SD_HOOK(M, fn, req) { #M, SD_IL2CPP_##M##_RVA, SD_IL2CPP_##M##_W0, SD_IL2CPP_##M##_W1, (void *)(fn), (req), SD_IL2CPP_##M##_ABSENT }

static int hooks_verify(uintptr_t base, const SdHook *h, int n, const char *set) {
  int ok = 1;
  for (int i = 0; i < n; i++) {
    if (!h[i].rva) {
      if (h[i].absent) {
        debugPrintf("[%s] %s: stripped from this build (nothing can call it) -- skipped\n", set, h[i].what);
      } else if (h[i].required) {
        debugPrintf("[%s] REQUIRED %s is not in this build\n", set, h[i].what);
        ok = 0;
      }
      continue;
    }
    const volatile uint32_t *p = (const volatile uint32_t *)(base + h[i].rva);
    if (p[0] != h[i].w0 || p[1] != h[i].w1) {
      debugPrintf("[%s] REFUSED %s @libil2cpp+0x%x: %08x %08x, expected %08x %08x\n",
                  set, h[i].what, h[i].rva, p[0], p[1], h[i].w0, h[i].w1);
      ok = 0;
    }
  }
  if (!ok)
    debugPrintf("[%s] NOT INSTALLED -- libil2cpp.so is not the build these offsets came "
                "from. Re-run tools/derive_il2cpp_hooks.py against your dump.cs.\n", set);
  return ok;
}

static int hooks_apply(uintptr_t base, const SdHook *h, int n) {
  int done = 0;
  for (int i = 0; i < n; i++)
    if (h[i].rva) { patch_jump(base + h[i].rva, h[i].fn); done++; }
  return done;
}

/* The game's own UI resolution, read from UnityEngine.Screen. Touch positions
 * must be in THIS space: when the game renders at a scaled size and Unity
 * software-blits it up to the window, Screen reports the scaled size and native
 * input converts touches into it -- a conversion our hooks bypass. Only called
 * once the game has polled touchCount, which proves il2cpp is running; the
 * getters' first two words are verified before the first call. */
static uintptr_t s_il2cpp_base;
int nx_input_game_screen(int *w, int *h) {
  static int ok = -1;                          /* -1 unchecked, 0 unusable, 1 verified */
  if (!s_il2cpp_base || !s_tc_calls) return 0;
  if (ok < 0) {
    const volatile uint32_t *pw = (const volatile uint32_t *)(s_il2cpp_base + SD_IL2CPP_SC_get_width_RVA);
    const volatile uint32_t *ph = (const volatile uint32_t *)(s_il2cpp_base + SD_IL2CPP_SC_get_height_RVA);
    ok = SD_IL2CPP_SC_get_width_RVA && SD_IL2CPP_SC_get_height_RVA &&
         pw[0] == SD_IL2CPP_SC_get_width_W0  && pw[1] == SD_IL2CPP_SC_get_width_W1 &&
         ph[0] == SD_IL2CPP_SC_get_height_W0 && ph[1] == SD_IL2CPP_SC_get_height_W1;
    if (!ok) debugPrintf("[touch] Screen getters failed their guards -- touch scale "
                         "falls back to the geometry request\n");
  }
  if (!ok) return 0;
  *w = ((int32_t (*)(void *))(s_il2cpp_base + SD_IL2CPP_SC_get_width_RVA))(NULL);
  *h = ((int32_t (*)(void *))(s_il2cpp_base + SD_IL2CPP_SC_get_height_RVA))(NULL);
  return *w > 0 && *h > 0;
}

void nx_install_input_hooks(uintptr_t il2cpp_base) {
  s_il2cpp_base = il2cpp_base;
  /* SD2 reads legacy UnityEngine.Input (it ships UnityEngine.InputLegacyModule and no UnityEngine.InputSystem
   * namespace in its dump). These are the whole of its touch input: every path to the native
   * touch icalls (touchCount, GetTouch, and the touches array that inlines both). */
  static const SdHook h[] = {
    SD_HOOK(IN_get_touchCount,     hk_touchCount,         1),
    SD_HOOK(IN_GetTouch,           hk_getTouch,           1),
    SD_HOOK(IN_get_mousePosition,  hk_mousePosition,      1),
    SD_HOOK(IN_GetMouseButton,     hk_getMouseButton,     1),
    SD_HOOK(IN_GetMouseButtonDown, hk_getMouseButtonDown, 1),
    SD_HOOK(IN_GetMouseButtonUp,   hk_getMouseButtonUp,   1),
    SD_HOOK(IN_get_touchSupported, hk_touchSupported,     0),
    SD_HOOK(IN_get_touches,        hk_getTouches,         0),   /* SimpleGestureMonitor: swipes */
  };
  const int n = (int)(sizeof h / sizeof h[0]);
  if (!hooks_verify(il2cpp_base, h, n, "input")) return;
  debugPrintf("[input] %d/%d Input hooks installed (touchCount/GetTouch/touches/mouse*)%s\n",
              hooks_apply(il2cpp_base, h, n), n,
              nx_input_il2cpp_api_ok() ? "" : " -- IL2CPP API missing: Input.touches will return null");

  /* Tilt: a separate set, so a mismatch here can never cost the touch hooks. */
  static const SdHook t[] = {
    SD_HOOK(IN_get_acceleration, hk_acceleration,     1),
    SD_HOOK(GY_set_enabled,      hk_gyro_set_enabled, 1),
    SD_HOOK(GY_get_enabled,      hk_gyro_get_enabled, 1),
    SD_HOOK(SI_supportsGyroscope, hk_supports_gyro,   1),
  };
  const int nt = (int)(sizeof t / sizeof t[0]);
  if (hooks_verify(il2cpp_base, t, nt, "tilt"))
    debugPrintf("[tilt] %d/%d hooks installed (Input.acceleration, Input.gyro.enabled, SystemInfo.supportsGyroscope) -- Enerbeam steering\n",
                hooks_apply(il2cpp_base, t, nt), nt);
}

/* Called by android_native_feed_hid with EVERY finger currently down, already mapped to Unity
 * screen space (bottom-left origin, game pixels). Phases are derived by matching HID finger ids
 * against last frame: a new id is Began, a known id is Moved (or Stationary if it didn't move),
 * and an id that disappeared is reported for exactly ONE more frame as Ended at its last
 * position -- Unity's contract, and what lets the game see a tap complete.
 * Fingers keep their slot order, so Input.GetTouch(i) is stable across frames. */
void nx_input_hook_update_multi(const NxTouchIn *in, int n) {
  static HookTouch prev[NX_MAX_TOUCH];
  static int prev_n = 0;
  HookTouch cur[NX_MAX_TOUCH];
  int cn = 0;

  if (n < 0) n = 0;
  if (n > NX_MAX_TOUCH) n = NX_MAX_TOUCH;

  /* 1) every finger that is down now: Began / Moved / Stationary */
  for (int i = 0; i < n; i++) {
    const HookTouch *was = NULL;
    for (int j = 0; j < prev_n; j++)
      if (prev[j].id == in[i].id && prev[j].phase != 3) { was = &prev[j]; break; }

    cur[cn].id = in[i].id;
    cur[cn].x  = in[i].x;
    cur[cn].y  = in[i].y;
    if (was) {
      float dx = in[i].x - was->x, dy = in[i].y - was->y;
      cur[cn].phase = (dx*dx + dy*dy > 0.25f) ? 1 /*Moved*/ : 2 /*Stationary*/;
      cur[cn].dx = dx; cur[cn].dy = dy;
    } else {
      cur[cn].phase = 0 /*Began*/;
      cur[cn].dx = cur[cn].dy = 0.0f;
      touch_log_hid("Began", in[i].id, in[i].x, in[i].y);
    }
    cn++;
  }

  /* 2) fingers that were down last frame and are now gone: one Ended frame each */
  for (int j = 0; j < prev_n && cn < NX_MAX_TOUCH; j++) {
    if (prev[j].phase == 3) continue;              /* already reported Ended -> drop it */
    int still_down = 0;
    for (int i = 0; i < n; i++)
      if (in[i].id == prev[j].id) { still_down = 1; break; }
    if (!still_down) {
      cur[cn] = prev[j];
      cur[cn].phase = 3 /*Ended*/;
      cur[cn].dx = cur[cn].dy = 0.0f;
      touch_log_hid("Ended", prev[j].id, prev[j].x, prev[j].y);
      cn++;
    }
  }

  /* 3) publish to the hooks */
  for (int i = 0; i < cn; i++) g_hook_touch[i] = cur[i];
  g_hook_count = cn;

  /* 4) mouse emulation mirrors the primary finger (unchanged behaviour for mouse-driven UI) */
  static int prev_active = 0;
  int active = (n > 0);
  g_hook_btn_down = (active && !prev_active);
  g_hook_btn_up   = (!active && prev_active);
  g_hook_btn      = active;
  if (cn > 0) {
    g_hook_x = cur[0].x; g_hook_y = cur[0].y;
    g_hook_phase = cur[0].phase;
  } else {
    g_hook_phase = 3;
  }
  prev_active = active;

  /* 5) remember this frame */
  for (int i = 0; i < cn; i++) prev[i] = cur[i];
  prev_n = cn;
}

/* Single-touch wrapper (stick-cursor / A-button path, which has only one pointer). */
void nx_input_hook_update(int active, float ux, float uy) {
  /* id 1000: deliberately outside the HID finger-id range (0..15) so switching between the
   * stick cursor and a real finger is seen as a new finger (Began), not a jump of an old one. */
  NxTouchIn t = { 1000, ux, uy };
  nx_input_hook_update_multi(active ? &t : NULL, active ? 1 : 0);
}

/* ---- PlayerPrefs persistence ---------------------------------------------
 * Saves go via UnityEngine.PlayerPrefs, but Unity's native PlayerPrefs never
 * reaches disk on Switch, so progress lived only in RAM. We replace Set/Get/Delete/Save so
 * the game's PlayerPrefs go through our persistent store (unity_jni.c prefs.kv), loaded on
 * boot and flushed to the SD. RVAs come from sd_il2cpp_offsets.h (this build's dump.cs). */
extern void        nx_prefs_set(char type, const char *key, const char *val);   /* unity_jni.c */
extern const char *nx_prefs_get(const char *key);
extern void        nx_prefs_del(const char *key);
extern void        nx_prefs_flush(void);

static void *(*g_il2cpp_string_new)(const char *);

/* il2cpp System.String (arm64): _stringLength @0x10 (int32), _firstChar (UTF-16) @0x14
 * (confirmed against dump.cs). Malloc'd UTF-8 copy (caller frees). */
static char *il2str_dup(void *s) {
  if (!s) return NULL;
  int32_t len = *(int32_t *)((char *)s + 0x10);
  if (len < 0) len = 0;
  char *out = (char *)malloc((size_t)len * 3 + 1);
  if (!out) return NULL;
  const uint16_t *ch = (const uint16_t *)((char *)s + 0x14);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = ch[i];
    if (c < 0x80)        out[o++] = (char)c;
    else if (c < 0x800){ out[o++] = (char)(0xC0|(c>>6));  out[o++] = (char)(0x80|(c&0x3F)); }
    else               { out[o++] = (char)(0xE0|(c>>12)); out[o++] = (char)(0x80|((c>>6)&0x3F)); out[o++] = (char)(0x80|(c&0x3F)); }
  }
  out[o] = 0;
  return out;
}
static void *mkstr(const char *s) { return g_il2cpp_string_new ? g_il2cpp_string_new(s ? s : "") : (void *)0; }

/* il2cpp static-method ABI: (real args..., MethodInfo*). */
static void hk_pp_SetString(void *key, void *val, void *mi) {
  (void)mi; char *k = il2str_dup(key), *v = il2str_dup(val);
  if (k) nx_prefs_set('S', k, v ? v : "");
  free(k); free(v);
}
static void hk_pp_SetInt(void *key, int32_t val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[16];
  if (k) { snprintf(b, sizeof b, "%d", (int)val); nx_prefs_set('I', k, b); }
  free(k);
}
static void hk_pp_SetFloat(void *key, float val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[32];
  if (k) { snprintf(b, sizeof b, "%.9g", (double)val); nx_prefs_set('F', k, b); }
  free(k);
}
static void *hk_pp_GetString2(void *key, void *def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? mkstr(v) : def;
}
static void *hk_pp_GetString1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return mkstr(v ? v : "");
}
static int32_t hk_pp_GetInt2(void *key, int32_t def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : def;
}
static int32_t hk_pp_GetInt1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : 0;
}
static float hk_pp_GetFloat2(void *key, float def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (float)atof(v) : def;
}
static uint8_t hk_pp_HasKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); int h = (k && nx_prefs_get(k)); free(k); return h ? 1 : 0;
}
static void hk_pp_DeleteKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); if (k) nx_prefs_del(k); free(k);
}
static void hk_pp_Save(void *mi) { (void)mi; nx_prefs_flush(); }

/* PlayerPrefs -> prefs.kv. Unity's Android PlayerPrefs goes through
 * SharedPreferences over the fake JNI and never reaches disk, so without these
 * hooks progress is lost on exit.
 *
 * ALL-OR-NOTHING, and stricter than the donor. The inherited code hooked the
 * setters even when it had to skip GetString (il2cpp_string_new unresolved) --
 * writes then went to prefs.kv while reads went to Unity's empty native store,
 * so the game could never read back its own save. Here, if string_new is
 * missing or any required target fails its guard, NONE are installed and the
 * game uses the native path consistently.
 *
 * GetInt(string) has no compiled body in this build (see sd_il2cpp_offsets.h),
 * so it is optional; its callers reach GetInt(string,int), which is hooked. */
void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new) {
  static const SdHook h[] = {
    SD_HOOK(PP_SetString,  hk_pp_SetString,  1), SD_HOOK(PP_SetInt,     hk_pp_SetInt,     1),
    SD_HOOK(PP_SetFloat,   hk_pp_SetFloat,   1), SD_HOOK(PP_GetString2, hk_pp_GetString2, 1),
    SD_HOOK(PP_GetString1, hk_pp_GetString1, 1), SD_HOOK(PP_GetInt2,    hk_pp_GetInt2,    1),
    SD_HOOK(PP_GetInt1,    hk_pp_GetInt1,    0), SD_HOOK(PP_GetFloat2,  hk_pp_GetFloat2,  1),
    SD_HOOK(PP_HasKey,     hk_pp_HasKey,     1), SD_HOOK(PP_DeleteKey,  hk_pp_DeleteKey,  1),
    SD_HOOK(PP_Save,       hk_pp_Save,       1),
  };
  const int n = (int)(sizeof h / sizeof h[0]);
  if (!string_new) {
    debugPrintf("[prefs] il2cpp_string_new unresolved -- NO prefs hooks (a partial set "
                "would split writes and reads across two stores)\n");
    return;
  }
  g_il2cpp_string_new = (void *(*)(const char *))string_new;
  if (!hooks_verify(il2cpp_base, h, n, "prefs")) return;
  debugPrintf("[prefs] %d PlayerPrefs hooks installed -> prefs.kv\n", hooks_apply(il2cpp_base, h, n));
}

/* ---- game tweaks: each its own hook set, each behind a config.h switch ------ */

/* AnalyticsSystem.CanAnalyticsEventsBeCollected(event): the gate every event
 * passes (CanPushAnalyticsEvent, CanPushAnalyticsEventToManualQueue). "No" is
 * the game's own opt-out path: nothing is queued, sent, failed -- or appended
 * to failed_events.log, which reached 187 KB in one offline session. */
static uint8_t hk_analyticsOff(void *self, void *ev, void *mi) {
  (void)self; (void)ev; (void)mi;
  return 0;
}

void nx_install_tweak_hooks(uintptr_t il2cpp_base) {
#if SD_ANALYTICS_OFF
  { static const SdHook h[] = { SD_HOOK(AN_CanCollect, hk_analyticsOff, 1) };
    if (hooks_verify(il2cpp_base, h, 1, "analytics"))
      debugPrintf("[tweak] analytics collection off (no failed_events.log): %d hook installed\n", hooks_apply(il2cpp_base, h, 1)); }
#endif
  (void)il2cpp_base;
}

/* ---- what managed code sees of the clock ------------------------------------
 * The swipe investigation raised the question whether SD2's managed time is
 * right (bouncemasters_nx hooked every UnityEngine.Time getter because, per
 * cloverpit_nx, TimeManager::Update never runs on Unity 6). This port instead
 * detours TimeManager::Update onto a monotonic clock and lets the engine do the
 * rest -- including timeScale -- so rather than assert, log what C# actually
 * reads, next to how often our detour ran. Called from main.c's loop; the
 * getters are only CALLED (never patched), after their guard words verify and
 * after the game has polled touchCount (proof that managed code is running). */
unsigned sd_tm_update_calls(void);   /* sd_patches.c */
void nx_log_managed_time(void) {
  static int ok = -1;
  static unsigned last_tm;
  if (!s_il2cpp_base || !s_tc_calls) return;
  if (ok < 0) {
    static const SdHook g[] = {
      SD_HOOK(TI_get_deltaTime, NULL, 1), SD_HOOK(TI_get_unscaledDeltaTime, NULL, 1),
      SD_HOOK(TI_get_realtimeSinceStartup, NULL, 1), SD_HOOK(TI_get_timeScale, NULL, 1),
      SD_HOOK(TI_get_frameCount, NULL, 1),
    };
    ok = hooks_verify(s_il2cpp_base, g, (int)(sizeof g / sizeof g[0]), "time-probe");
    if (!ok) return;
  }
  if (!ok) return;
  const uintptr_t b = s_il2cpp_base;
  const float dt  = ((float   (*)(void *))(b + SD_IL2CPP_TI_get_deltaTime_RVA))(NULL);
  const float udt = ((float   (*)(void *))(b + SD_IL2CPP_TI_get_unscaledDeltaTime_RVA))(NULL);
  const float rt  = ((float   (*)(void *))(b + SD_IL2CPP_TI_get_realtimeSinceStartup_RVA))(NULL);
  const float ts  = ((float   (*)(void *))(b + SD_IL2CPP_TI_get_timeScale_RVA))(NULL);
  const int   fc  = ((int32_t (*)(void *))(b + SD_IL2CPP_TI_get_frameCount_RVA))(NULL);
  const unsigned tm = sd_tm_update_calls();
  debugPrintf("[time] managed: deltaTime=%.2f ms unscaled=%.2f ms timeScale=%.2f frame=%d "
              "realtime=%.2fs | TimeManager::Update detour ran %u times (+%u)\n",
              dt * 1000.0f, udt * 1000.0f, ts, fc, rt, tm, tm - last_tm);
  last_tm = tm;
}
