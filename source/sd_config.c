/* sd_config.c -- config.txt: the player's settings, read at every launch.
 *
 * Modelled on bloonspop_nx's bp_config.c. The file lives next to the .nro
 * (GAME_HOME) and is written, documented inline, on first launch. A config.txt
 * from an older build that lacks an option gets that option's block appended,
 * so new settings appear without the player deleting the file.
 *
 *   resolution  the height of the output picture in pixels, 720 (default) ..
 *               1080. One setting for handheld and docked -- the Switch scales
 *               the picture to the panel or the TV. Snapped to a multiple of 18
 *               so the 16:9 window is exact (bloonspop_nx's rule).
 *   rotation    0 = landscape 16:9, full screen (default) -- as Sonic Dash 1.
 *               SD2 declares portrait only, but is played landscape here.
 *               Portrait (sd_tate.c): 1 = turned 90 clockwise, 2 = 90 counter-
 *               clockwise -- console held vertically, the whole panel;
 *               3 = upright in the middle of the screen, black sides.
 *   language    auto, or one of the languages the game ships: en fr de it pt
 *               ru es. auto follows the Switch's language when the game has it,
 *               and falls back to English otherwise (bloonspop_nx dropped its
 *               option after the console's language handed its game one it
 *               could not render; restricting auto to the shipped set is the
 *               fix, not removing the choice).
 * MIT.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "config.h"
#include "sd_config.h"
#include "util.h"

int sd_cfg_res      = 720;
int sd_cfg_rotation = 0;
int sd_cfg_tilt = 3;          /* SD_TILT_BOTH */
int sd_cfg_tilt_invert = 0;
int sd_cfg_tilt_sens = 100;
/* The diagnostic tracers follow DEBUG_LOG (config.h) -- they only write to
 * debug.log. They are not config.txt options any more. With DEBUG_LOG 1 the
 * UI trace is on and the JNI trace is "normal"; set sd_cfg_jni_log to 2 here
 * for "extensive" (every Java call). */
int sd_cfg_ui_trace = DEBUG_LOG ? 1 : 0;
int sd_cfg_jni_log = DEBUG_LOG ? 1 : 0;
static char s_lang[8] = "auto";

/* The languages Sonic Dash 2 ships: English, French, German, Italian,
 * Portuguese, Russian, Spanish. (Hardlight's shared HLLanguageState.Language
 * enum in the game's code lists more -- Japanese, Korean, Chinese -- but those
 * are the framework's, not this game's localisations.) */
static const struct { const char *code, *name; } k_langs[] = {
  { "en", "english" }, { "fr", "french" },     { "de", "german" }, { "it", "italian" },
  { "pt", "portuguese" }, { "ru", "russian" }, { "es", "spanish" },
};

static const struct { const char *key; const char *block; } SECTIONS[] = {
  { "resolution",
    "# --- resolution --------------------------------------------------------\n"
    "# The picture's height, in pixels, and what the game renders:\n"
    "#            rotation 0     rotation 1/2   rotation 3\n"
    "#    720  -> 1280 x  720    720 x 1280     405 x  720   (default)\n"
    "#    900  -> 1600 x  900    900 x 1600     506 x  900\n"
    "#   1080  -> 1920 x 1080   1080 x 1920     608 x 1080\n"
    "# Any value from 720 to 1080 is accepted and rounded to the nearest size\n"
    "# that keeps the screen exactly 16:9. One setting for both handheld and\n"
    "# docked. Higher is sharper, most visibly on a TV, but costs performance.\n"
    "resolution = 720\n" },
  { "rotation",
    "# --- rotation ----------------------------------------------------------\n"
    "#   0 = landscape, full screen (default)\n"
    "#   1 = portrait, turned 90 degrees clockwise: hold the console\n"
    "#       vertically, right Joy-Con up -- uses the whole panel\n"
    "#   2 = portrait, turned 90 degrees counter-clockwise (left Joy-Con up)\n"
    "#   3 = portrait, upright in the middle of the screen, black sides\n"
    "rotation = 0\n" },
  { "language",
    "# --- language ----------------------------------------------------------\n"
    "# auto follows the Switch's language when the game has it, else English.\n"
    "# Or pick one: en (English), fr (French), de (German), it (Italian),\n"
    "# pt (Portuguese), ru (Russian), es (Spanish).\n"
    "language = auto\n" },
  { "tilt",
    "# --- tilt (Enerbeam) -----------------------------------------------------\n"
    "# The Enerbeam swings the way you lean, like tilting a phone. While a beam\n"
    "# is active, tilt the console or controller left and right (like a\n"
    "# steering wheel), or push the left stick. The pose you hold when a beam\n"
    "# starts counts as straight ahead.\n"
    "#   both  = motion sensor, or the left stick while it is pushed (default)\n"
    "#   gyro  = motion sensor only    stick = left stick only    off\n"
    "tilt = both\n"
    "# 1 swaps left and right (if leaning right swings you left).\n"
    "tilt_invert = 0\n"
    "# How strongly a lean counts, in percent (25 to 400).\n"
    "tilt_sensitivity = 100\n" },
};

static void write_header(FILE *f) {
  fputs("# config.txt -- Sonic Dash 2: Sonic Boom (sonicboomdash) settings, read at every launch.\n"
        "# Edit a value after the '='; anything after a '#' is a comment.\n\n", f);
}

/* Is there a line "key =" or "#key =" (any spacing)? Prose that merely
 * mentions the word does not count. */
static int has_key_line(const char *text, const char *key) {
  const size_t kl = strlen(key);
  for (const char *p = text; *p; ) {
    const char *q = p;
    while (*q == ' ' || *q == '\t' || *q == '#') q++;
    if (!strncmp(q, key, kl)) {
      const char *r = q + kl;
      while (*r == ' ' || *r == '\t') r++;
      if (*r == '=') return 1;
    }
    const char *nl = strchr(p, '\n');
    if (!nl) break;
    p = nl + 1;
  }
  return 0;
}

/* ---- retired options --------------------------------------------------------
 * ui_trace and jni_trace (and the older jni_log) are no longer config.txt
 * options. An existing file loses each one's key line and the comment lines
 * this port wrote directly above it (every version of those blocks); a generic
 * comment line elsewhere is never touched, and every other line is kept exactly. */
static const char *const k_retired_keys[] = { "ui_trace", "jni_trace", "jni_log" };
static const char *const k_retired_comments[] = {   /* [0] and [3] open a block */
  "# --- diagnostics ---------------------------------------------------------",
  "# 1 logs, for every tap, what was under your finger and whether it became",
  "# a click (debug.log lines starting [ui]). Harmless; 0 turns it off.",
  "# What the game asks of Android (Java) and what it is told, in debug.log:",
  "#   normal    = each Java method's first few calls (default)",
  "#   normal    = each Java method's first few calls only",
  "#   extensive = every call, with its arguments and the answer ([jnicall])",
  "#   off",
};
static int line_is(const char *ln, size_t len, const char *const *set, size_t n) {
  while (len && (ln[len - 1] == '\r' || ln[len - 1] == ' ' || ln[len - 1] == '\t')) len--;
  for (size_t i = 0; i < n; i++) if (strlen(set[i]) == len && !memcmp(ln, set[i], len)) return 1;
  return 0;
}
static int line_is_retired_key(const char *ln, size_t len) {
  const char *q = ln, *e = ln + len;
  while (q < e && (*q == ' ' || *q == '\t' || *q == '#')) q++;
  for (size_t i = 0; i < sizeof k_retired_keys / sizeof *k_retired_keys; i++) {
    const size_t kl = strlen(k_retired_keys[i]);
    if ((size_t)(e - q) > kl && !strncmp(q, k_retired_keys[i], kl)) {
      const char *r = q + kl; while (r < e && (*r == ' ' || *r == '\t')) r++;
      if (r < e && *r == '=') return 1;
    }
  }
  return 0;
}
/* Returns 1 if `text` held a retired option (and the file was rewritten without it). */
static int retire_options(const char *path, const char *text) {
  size_t nl = 1; for (const char *p = text; *p; p++) nl += *p == '\n';
  const char **ls = malloc(nl * sizeof *ls); size_t *ll = malloc(nl * sizeof *ll); char *drop = calloc(nl, 1);
  if (!ls || !ll || !drop) { free(ls); free(ll); free(drop); return 0; }
  size_t n = 0;
  for (const char *p = text; ; ) {
    const char *e = strchr(p, '\n');
    ls[n] = p; ll[n] = e ? (size_t)(e - p) : strlen(p); n++;
    if (!e) break;
    p = e + 1;
  }
  int found = 0;
  for (size_t i = 0; i < n; i++) {
    if (!line_is_retired_key(ls[i], ll[i])) continue;
    found = 1; drop[i] = 1;
    size_t j = i;
    /* up through the block's own comments, stopping at its first line: a
     * matching comment of the player's above the block is not part of it */
    while (j > 0 && line_is(ls[j - 1], ll[j - 1], k_retired_comments, sizeof k_retired_comments / sizeof *k_retired_comments)) {
      drop[--j] = 1;
      if (line_is(ls[j], ll[j], k_retired_comments, 1) || line_is(ls[j], ll[j], k_retired_comments + 3, 1)) break;
    }
    /* the blank line that separated the block goes with it */
    if (i + 1 < n && ll[i + 1] == 0 && (j == 0 || ll[j - 1] == 0 || drop[j - 1])) drop[i + 1] = 1;
  }
  int ok = 0;
  if (found) {
    char tmp[320]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
      int first = 1;
      for (size_t i = 0; i < n; i++) {
        if (drop[i]) continue;
        if (i == n - 1 && ll[i] == 0) break;              /* the text's final newline */
        if (!first) fputc('\n', f);
        fwrite(ls[i], 1, ll[i], f); first = 0;
      }
      if (!first) fputc('\n', f);
      if (fclose(f) == 0) { remove(path); ok = rename(tmp, path) == 0; }  /* FAT: rename does not replace */
      if (!ok) remove(tmp);
    }
    debugPrintf("[config] %s the retired diagnostic options (ui_trace, jni_trace) from %s\n", ok ? "removed" : "could not remove", path);
  }
  free(ls); free(ll); free(drop);
  return ok;
}

static char *trim(char *p) {
  while (isspace((unsigned char)*p)) p++;
  char *e = p + strlen(p);
  while (e > p && isspace((unsigned char)e[-1])) *--e = 0;
  return p;
}

static void apply(const char *key, const char *val) {
  if (!strcmp(key, "resolution")) {
    char *end; long v = strtol(val, &end, 10);
    if (*end || end == val) { debugPrintf("[config] resolution \"%s\" is not a number -- using %d\n", val, sd_cfg_res); return; }
    if (v < 720) v = 720;
    if (v > 1080) v = 1080;
    long s = ((v + 9) / 18) * 18;                          /* nearest exact 16:9 size */
    if (s != v) debugPrintf("[config] resolution %ld -> %ld (nearest exact 16:9 size)\n", v, s);
    sd_cfg_res = (int)s;
  } else if (!strcmp(key, "rotation")) {
    if (val[0] >= '0' && val[0] <= '3' && !val[1]) sd_cfg_rotation = val[0] - '0';
    else debugPrintf("[config] rotation \"%s\" -- use 0, 1, 2 or 3; keeping %d\n", val, sd_cfg_rotation);
  } else if (!strcmp(key, "language")) {
    char v[16]; size_t i = 0;
    for (; val[i] && i < sizeof v - 1; i++) v[i] = (char)tolower((unsigned char)val[i]);
    v[i] = 0;
    if (!strcmp(v, "auto")) { strcpy(s_lang, "auto"); return; }
    for (size_t k = 0; k < sizeof k_langs / sizeof k_langs[0]; k++)
      if (!strcmp(v, k_langs[k].code) || !strcmp(v, k_langs[k].name)) { strcpy(s_lang, k_langs[k].code); return; }
    strcpy(s_lang, "auto");                                /* do what the log says */
    debugPrintf("[config] language \"%s\" is not one the game ships (en fr de it pt ru es) -- using auto\n", val);
  } else if (!strcmp(key, "tilt")) {
    if      (!strcasecmp(val, "both"))  sd_cfg_tilt = 3;
    else if (!strcasecmp(val, "gyro"))  sd_cfg_tilt = 1;
    else if (!strcasecmp(val, "stick")) sd_cfg_tilt = 2;
    else if (!strcasecmp(val, "off"))   sd_cfg_tilt = 0;
    else debugPrintf("[config] tilt \"%s\" -- use both, gyro, stick or off; keeping %d\n", val, sd_cfg_tilt);
  } else if (!strcmp(key, "tilt_invert")) {
    sd_cfg_tilt_invert = (val[0] == '1' && !val[1]);
  } else if (!strcmp(key, "tilt_sensitivity")) {
    char *end; long v = strtol(val, &end, 10);
    if (*end || end == val) debugPrintf("[config] tilt_sensitivity \"%s\" is not a number -- keeping %d\n", val, sd_cfg_tilt_sens);
    else sd_cfg_tilt_sens = (int)(v < 25 ? 25 : v > 400 ? 400 : v);
  } else if (!strcmp(key, "ui_trace") || !strcmp(key, "jni_trace") || !strcmp(key, "jni_log")) {
    /* Retired from config.txt (the tracers follow DEBUG_LOG); retire_options()
     * removes the line, so this is only reached if removing it failed. */
  } else if (!strcmp(key, "screen_width") || !strcmp(key, "screen_height")) {
    debugPrintf("[config] %s is from an older build and is ignored -- use resolution\n", key);
  } else {
    debugPrintf("[config] unknown setting \"%s\" -- ignored\n", key);
  }
}

void sd_config_load(void) {
  char path[300];
  snprintf(path, sizeof path, "%s/config.txt", GAME_HOME);
  char *text = NULL; long n = 0;
  FILE *f = fopen(path, "rb");
  if (f) {
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (1 << 20) && (text = malloc((size_t)n + 1))) {
      n = (long)fread(text, 1, (size_t)n, f); text[n] = 0;
    }
    fclose(f);
  }
  if (!text) {                                             /* first launch: full template */
    f = fopen(path, "w");
    if (f) {
      write_header(f);
      for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++) { fputs(SECTIONS[i].block, f); fputs("\n", f); }
      fclose(f);
      debugPrintf("[config] wrote %s\n", path);
    }
  } else {
    retire_options(path, text);                            /* older file: drop retired options */
    int appended = 0;                                      /* ... and add what it lacks */
    for (size_t i = 0; i < sizeof SECTIONS / sizeof SECTIONS[0]; i++) {
      if (has_key_line(text, SECTIONS[i].key)) continue;
      if (!appended && !(f = fopen(path, "a"))) break;
      if (!appended) fputs("\n", f);
      fputs(SECTIONS[i].block, f); fputs("\n", f);
      appended++;
    }
    if (appended) { fclose(f); debugPrintf("[config] added %d new option(s) to %s\n", appended, path); }
    for (char *ln = strtok(text, "\n"); ln; ln = strtok(NULL, "\n")) {
      char *hash = strchr(ln, '#');
      if (hash) *hash = 0;                                 /* inline comments allowed */
      char *eq = strchr(ln, '=');
      if (!eq) continue;
      *eq = 0;
      char *k = trim(ln), *v = trim(eq + 1);
      if (*k && *v) apply(k, v);
    }
    free(text);
  }
  debugPrintf("[config] resolution %dp, rotation %d, language %s (-> %s), tilt %s%s %d%%\n",
              sd_cfg_res, sd_cfg_rotation, s_lang, sd_lang(),
              (const char *[]){ "off", "gyro", "stick", "both" }[sd_cfg_tilt & 3],
              sd_cfg_tilt_invert ? " inverted" : "", sd_cfg_tilt_sens);
  debugPrintf("[config] diagnostics (follow DEBUG_LOG): ui trace %d, jni trace %s\n", sd_cfg_ui_trace,
              (const char *[]){ "off", "normal", "extensive" }[sd_cfg_jni_log % 3]);
}

/* The two-letter language the whole port reports (Java Locale, SEGA's SLGlobal,
 * Hardlight's HLUnityCore). One answer, so the layers cannot disagree. */
const char *sd_lang(void) {
  if (strcmp(s_lang, "auto")) return s_lang;
  static char a[3];
  if (!a[0]) {
    char loc[9] = { 0 };
    u64 code = 0;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code))) memcpy(loc, &code, 8);
      setExit();
    }
    a[0] = (char)tolower((unsigned char)loc[0]);
    a[1] = (char)tolower((unsigned char)loc[1]);
    int ok = 0;
    for (size_t k = 0; k < sizeof k_langs / sizeof k_langs[0]; k++) ok |= !strcmp(a, k_langs[k].code);
    if (!ok) { a[0] = 'e'; a[1] = 'n'; }                   /* ja zh ko nl ... -> English */
  }
  return a;
}
