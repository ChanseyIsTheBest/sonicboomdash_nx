/* sd_uitrace.c -- log how each tap resolves into (or fails to become) a click.
 *
 * FOUND ON HARDWARE: in the character-select tutorial, tapping Amy highlights
 * her icon but nothing follows. CharacterSelectionButton.OnClick would, for a
 * locked character, set the selected character (the bottom panel would show
 * Amy -- it still shows Sonic) and open a page; so OnClick never ran, and no
 * exception was logged. The chain that broke is uGUI's:
 *
 *   ProcessTouchPress(pressed)  -> pointerPress / pointerClick / pointerDrag chosen
 *   ProcessTouchPress(released) -> click if pointerClick is the click handler over
 *                                  the release point AND eligibleForClick
 *                                  (cleared if a drag started) -> Button.Press()
 *   Button.Press -> onClick -> CharacterSelectionButton.OnClick -> DialogSystem.ShowPage
 *
 * Each link is OBSERVED here (sd_tramp.c: the method still runs unchanged) and
 * logged, so the next debug.log names the first link that did not happen and
 * why: what was under the finger at press and release, which objects uGUI chose,
 * whether the press was still eligible, whether a drag had started, and how far
 * the finger travelled against the drag threshold. Taps only -- no per-frame lines.
 * ------------------------------------------------------------------------- */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "config.h"
#include "sd_config.h"
#include "sd_il2cpp_offsets.h"
#include "sd_tramp.h"
#include "util.h"

void sd_jni_tap_window(unsigned tap);   /* sd_jni.c: log every JNI call for 2 s */

static uintptr_t s_base;
static unsigned s_taps;

#define FLD(T, p, off) (*(const T *)((const char *)(p) + (off)))

/* A UnityEngine.Object's name. Only for live objects: m_CachedPtr is the native
 * object, and asking a destroyed one for its name would throw. */
static const char *obj_name(const void *obj, char *buf, size_t n) {
  if (!obj) return "-";
  if (!FLD(void *, obj, SD_FLD_OB_cachedPtr)) return "(destroyed)";
  const char *(*get_name)(const void *, void *) = (const char *(*)(const void *, void *))(s_base + SD_IL2CPP_OB_get_name_RVA);
  const char *s = get_name(obj, NULL);
  if (!s) return "(no name)";
  const int32_t len = FLD(int32_t, s, 0x10);             /* Il2CppString: length, then UTF-16 */
  const uint16_t *c = (const uint16_t *)(s + 0x14);
  size_t o = 0;
  for (int32_t i = 0; i < len && o + 1 < n; i++) buf[o++] = (c[i] >= 0x20 && c[i] < 0x7F) ? (char)c[i] : '?';
  buf[o] = 0;
  return buf;
}

/* (module, PointerEventData pe, bool pressed, bool released) */
static void obs_touch_press(const uint64_t *x) {
  const void *pe = (const void *)x[1];
  const int pressed = (int)(x[2] & 0xFF), released = (int)(x[3] & 0xFF);
  if (!pe || (!pressed && !released)) return;
  const int id = FLD(int32_t, pe, SD_FLD_PED_pointerId);
  const float px = FLD(float, pe, SD_FLD_PED_position), py = FLD(float, pe, SD_FLD_PED_position + 4);
  const void *over = FLD(void *, pe, SD_FLD_PED_currentRaycast + SD_FLD_RR_gameObject);
  char a[96], b[96], c[96], d[96];
  if (pressed || released) sd_jni_tap_window(pressed ? s_taps + 1 : s_taps);
  if (pressed) {
    const void *es = FLD(void *, (const void *)x[0], SD_FLD_BIM_eventSystem);
    debugPrintf("[ui] tap %u PRESS   finger %d at (%.0f,%.0f) over '%s'  (drag threshold %d px)\n",
                ++s_taps, id, (double)px, (double)py, obj_name(over, a, sizeof a),
                es ? FLD(int32_t, es, SD_FLD_ES_dragThreshold) : -1);
  }
  if (released) {
    const float sx = FLD(float, pe, SD_FLD_PED_pressPosition), sy = FLD(float, pe, SD_FLD_PED_pressPosition + 4);
    debugPrintf("[ui] tap %u RELEASE finger %d at (%.0f,%.0f) over '%s' | pressed '%s' click-target '%s' "
                "drag-target '%s' | eligibleForClick %d dragging %d useDragThreshold %d travel %.1f px\n",
                s_taps, id, (double)px, (double)py, obj_name(over, a, sizeof a),
                obj_name(FLD(void *, pe, SD_FLD_PED_pointerPress), b, sizeof b),
                obj_name(FLD(void *, pe, SD_FLD_PED_pointerClick), c, sizeof c),
                obj_name(FLD(void *, pe, SD_FLD_PED_pointerDrag), d, sizeof d),
                FLD(uint8_t, pe, SD_FLD_PED_eligibleForClick), FLD(uint8_t, pe, SD_FLD_PED_dragging),
                FLD(uint8_t, pe, SD_FLD_PED_useDragThreshold), (double)hypotf(px - sx, py - sy));
  }
}
static void obs_button_press(const uint64_t *x) {        /* Button.Press(this) */
  char a[96];
  debugPrintf("[ui]   -> Button '%s' Press(): onClick runs if it is active and interactable\n",
              obj_name((const void *)x[0], a, sizeof a));
}
static void obs_csb_onclick(const uint64_t *x) {         /* CharacterSelectionButton.OnClick(this) */
  static const char *const k[] = { "None", "Sonic", "Tails", "Knuckles", "Amy", "Sticks", "Shadow", "Vector" };
  const int t = FLD(int32_t, (const void *)x[0], SD_FLD_CSB_character);
  debugPrintf("[ui]   -> CharacterSelectionButton.OnClick: %s (%d)\n", t >= 0 && t < 8 ? k[t] : "?", t);
}
static void obs_showpage(const uint64_t *x) {            /* DialogSystem.ShowPage(this, id, data, cb) */
  debugPrintf("[ui]   -> DialogSystem.ShowPage(page %d)\n", (int)(int32_t)x[1]);
}
static void obs_purchase(const uint64_t *x) {
  (void)x; debugPrintf("[ui]   -> CharacterSelectPage.PurchaseCharacter()\n");
}

void sd_uitrace_install(so_module *il2cpp) {
  if (!sd_cfg_ui_trace) return;                     /* off unless DEBUG_LOG: nothing to write to */
  s_base = (uintptr_t)il2cpp->load_virtbase;
  static const struct { uintptr_t rva; uint32_t w0, w1; void (*obs)(const uint64_t *); const char *name; } t[] = {
    { SD_IL2CPP_UI_SIM_ProcessTouchPress_RVA, SD_IL2CPP_UI_SIM_ProcessTouchPress_W0, SD_IL2CPP_UI_SIM_ProcessTouchPress_W1,
      obs_touch_press, "StandaloneInputModule.ProcessTouchPress" },
    { SD_IL2CPP_UI_TIM_ProcessTouchPress_RVA, SD_IL2CPP_UI_TIM_ProcessTouchPress_W0, SD_IL2CPP_UI_TIM_ProcessTouchPress_W1,
      obs_touch_press, "TouchInputModule.ProcessTouchPress" },
    { SD_IL2CPP_UI_Button_Press_RVA, SD_IL2CPP_UI_Button_Press_W0, SD_IL2CPP_UI_Button_Press_W1,
      obs_button_press, "Button.Press" },
    { SD_IL2CPP_UI_CSB_OnClick_RVA, SD_IL2CPP_UI_CSB_OnClick_W0, SD_IL2CPP_UI_CSB_OnClick_W1,
      obs_csb_onclick, "CharacterSelectionButton.OnClick" },
    { SD_IL2CPP_UI_Dialog_ShowPage_RVA, SD_IL2CPP_UI_Dialog_ShowPage_W0, SD_IL2CPP_UI_Dialog_ShowPage_W1,
      obs_showpage, "DialogSystem.ShowPage" },
    { SD_IL2CPP_UI_CSP_PurchaseCharacter_RVA, SD_IL2CPP_UI_CSP_PurchaseCharacter_W0, SD_IL2CPP_UI_CSP_PurchaseCharacter_W1,
      obs_purchase, "CharacterSelectPage.PurchaseCharacter" },
  };
  const int n = (int)(sizeof t / sizeof t[0]);
  int ok = 0;
  for (int i = 0; i < n; i++) ok += sd_tramp_observe(il2cpp, s_base + t[i].rva, t[i].w0, t[i].w1, t[i].obs, t[i].name);
  debugPrintf("[ui] %d/%d observers installed -- every tap logs PRESS/RELEASE and what it reached\n", ok, n);
}
