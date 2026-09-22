/* sd_saveedit.c -- edit Sonic Dash 2's save at boot, from save_edit.txt.
 *
 * THE FORMAT (every part read out of this build's own code, not guessed)
 * -----------------------------------------------------------------------
 * File <game folder>/save (and save-backup, which the game keeps identical):
 *     u8  SaveVersion (3)      i32 LE  ciphertext length      ciphertext
 * AES-128-CBC (AesManaged defaults: CBC; padding with zero bytes):
 *   key = first 16 characters of Base64(UTF8("S0NiCda5hBo0M"))  -- HLPropertyStore.set_Key,
 *         the key string set by SonicDash.Loaders.EntryPoint.NotifyGame
 *   IV  = UTF8("7%o18m'b~)mQD%o1")                               -- HLPropertyStore..cctor
 * Plaintext: "0\n<count>\n" then <count> pairs "<nameCRC>\n<value>\n", where
 * nameCRC is Hardlight.HLCRC32.Generate(name, Case.Lower): CRC-32 table
 * 0xEDB88320, init 0xFFFFFFFF, NO final xor, over the lower-cased name.
 *
 * The player's state is EVENT-SOURCED (the "PSM" properties): a base snapshot
 * PSM_BasePlayerState plus PSM_TransactionCount transactions stored as
 * PSM_TransactionCount_0.._N-1, each JSON with key/value additions, updates and
 * removals and matching currency deltas. The state the game shows is the base
 * with every transaction replayed.
 *
 * So an edit is made the way the game makes one: ONE new transaction appended,
 * its deltas agreeing with its values. Nothing already in the save is rewritten.
 *
 * HOW IT IS USED (sonicdash_nx's design): save_edit.txt is written next to the
 * save by the game, once, every setting commented out. An uncommented setting
 * is applied at EVERY launch while it stays uncommented -- "rings = 50000"
 * puts you back at 50000 each launch; comment it out once it has taken. When
 * the save already holds every requested value nothing is written, so leaving
 * a line uncommented costs nothing. What each launch did goes to debug.log
 * ([saveedit]) and save_edit_result.txt.
 *
 * SAFETY: anything unexpected -- header, length, padding, count, a transaction
 * that does not parse -- aborts with the save untouched. The new plaintext is
 * re-parsed and replayed and every edited value checked, and the ciphertext
 * decrypted back and compared, before a byte is written. The previous save is
 * kept as save.before-edit (and the very first one as save.original).
 * ------------------------------------------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "sd_saveedit.h"

#define SE_KEY_STRING  "S0NiCda5hBo0M"
#define SE_IV_STRING   "7%o18m'b~)mQD%o1"
#define SE_VERSION     3
#define SE_MAX_FILE    (4u << 20)

/* ------------------------------------------------------------ report ---- */
static char  s_report[8192];
static size_t s_rlen;
static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char *fmt, ...) {
  char line[512]; va_list va; va_start(va, fmt); vsnprintf(line, sizeof line, fmt, va); va_end(va);
  debugPrintf("[saveedit] %s\n", line);
  int w = snprintf(s_report + s_rlen, sizeof s_report - s_rlen, "#   %s\n", line);
  if (w > 0 && s_rlen + (size_t)w < sizeof s_report) s_rlen += (size_t)w;
}

/* ------------------------------------------------------ key and CRC ---- */
static void derive_key(uint8_t key[16]) {
  static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t *in = (const uint8_t *)SE_KEY_STRING; const size_t n = strlen(SE_KEY_STRING);
  char out[64]; size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = b64[(v >> 18) & 63]; out[o++] = b64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? b64[v & 63] : '=';
  }
  memcpy(key, out, 16);                                   /* "UzBOaUNkYTVoQm8w" */
}

uint32_t sd_saveedit_crc(const char *name) {             /* HLCRC32.Generate(name, Case.Lower) */
  static uint32_t t[256]; static int ready;
  if (!ready) {
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; t[i] = c; }
    ready = 1;
  }
  if (!name || !*name) return 0;
  uint32_t c = 0xFFFFFFFFu;
  for (const char *p = name; *p; p++) c = t[(c ^ (uint8_t)tolower((unsigned char)*p)) & 0xFF] ^ (c >> 8);
  return c;
}

static char *dupn(const char *s, size_t n) {
  char *d = malloc(n + 1); if (d) { memcpy(d, s, n); d[n] = 0; } return d;
}

/* ------------------------------------------------------ properties ---- */
typedef struct { uint32_t crc; char *val; } Prop;
typedef struct { char *head; Prop *p; int n, cap; } Props;

static void props_free(Props *ps) {
  for (int i = 0; i < ps->n; i++) free(ps->p[i].val);
  free(ps->p); free(ps->head); memset(ps, 0, sizeof *ps);
}
static int props_insert(Props *ps, int at, uint32_t crc, const char *val) {
  if (ps->n == ps->cap) {
    int nc = ps->cap ? ps->cap * 2 : 256; Prop *np = realloc(ps->p, (size_t)nc * sizeof *np);
    if (!np) return 0;
    ps->p = np; ps->cap = nc;
  }
  memmove(&ps->p[at + 1], &ps->p[at], (size_t)(ps->n - at) * sizeof *ps->p);
  ps->p[at].crc = crc; ps->p[at].val = strdup(val); ps->n++;
  return ps->p[at].val != NULL;
}
static int props_find(const Props *ps, const char *name) {
  const uint32_t c = sd_saveedit_crc(name);
  for (int i = 0; i < ps->n; i++) if (ps->p[i].crc == c) return i;
  return -1;
}

/* "0\n<count>\n" then pairs. `len` excludes the zero padding. */
static int props_parse(Props *ps, const char *text, size_t len) {
  memset(ps, 0, sizeof *ps);
  const char *p = text, *end = text + len;
  const char *nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) return 0;
  ps->head = dupn(p, (size_t)(nl - p)); p = nl + 1;
  nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) return 0;
  char num[24]; size_t nlen = (size_t)(nl - p); if (!nlen || nlen >= sizeof num) return 0;
  memcpy(num, p, nlen); num[nlen] = 0; p = nl + 1;
  char *e; long cnt = strtol(num, &e, 10); if (*e || cnt < 0 || cnt > 100000) return 0;
  for (long i = 0; i < cnt; i++) {
    nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) return 0;
    nlen = (size_t)(nl - p); if (!nlen || nlen >= sizeof num) return 0;
    memcpy(num, p, nlen); num[nlen] = 0; p = nl + 1;
    unsigned long long crc = strtoull(num, &e, 10); if (*e || crc > 0xFFFFFFFFull) return 0;
    nl = memchr(p, '\n', (size_t)(end - p)); if (!nl) return 0;
    char *v = dupn(p, (size_t)(nl - p)); p = nl + 1;
    if (!v || !props_insert(ps, ps->n, (uint32_t)crc, v)) { free(v); return 0; }
    free(v);
  }
  return p == end;                                        /* nothing may follow the last pair */
}

static char *props_serialize(const Props *ps, size_t *out_len) {
  size_t cap = strlen(ps->head) + 32;
  for (int i = 0; i < ps->n; i++) cap += strlen(ps->p[i].val) + 16;
  char *buf = malloc(cap + 16); if (!buf) return NULL;
  size_t o = (size_t)snprintf(buf, cap, "%s\n%d\n", ps->head, ps->n);
  for (int i = 0; i < ps->n; i++) o += (size_t)snprintf(buf + o, cap - o, "%u\n%s\n", ps->p[i].crc, ps->p[i].val);
  *out_len = o;
  return buf;
}

/* ----------------------------------------------------- player state ---- */
typedef struct { char *k, *v; } KV;
typedef struct { KV *a; int n, cap; } State;

static void state_free(State *s) { for (int i = 0; i < s->n; i++) { free(s->a[i].k); free(s->a[i].v); } free(s->a); memset(s, 0, sizeof *s); }
static int state_idx(const State *s, const char *k) { for (int i = 0; i < s->n; i++) if (!strcmp(s->a[i].k, k)) return i; return -1; }
static const char *state_get(const State *s, const char *k) { int i = state_idx(s, k); return i < 0 ? NULL : s->a[i].v; }
static int state_set(State *s, const char *k, const char *v) {
  int i = state_idx(s, k);
  if (i >= 0) { char *nv = strdup(v); if (!nv) return 0; free(s->a[i].v); s->a[i].v = nv; return 1; }
  if (s->n == s->cap) { int nc = s->cap ? s->cap * 2 : 128; KV *na = realloc(s->a, (size_t)nc * sizeof *na); if (!na) return 0; s->a = na; s->cap = nc; }
  s->a[s->n].k = strdup(k); s->a[s->n].v = strdup(v);
  if (!s->a[s->n].k || !s->a[s->n].v) return 0;
  s->n++; return 1;
}
static void state_del(State *s, const char *k) {
  int i = state_idx(s, k); if (i < 0) return;
  free(s->a[i].k); free(s->a[i].v); s->a[i] = s->a[--s->n];
}

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

/* A JSON string at *pp (at its opening quote) -> out (UTF-8). */
static int json_str(const char **pp, char *out, size_t n) {
  const char *p = *pp; size_t o = 0;
  if (*p++ != '"') return 0;
  while (*p && *p != '"') {
    unsigned c = (unsigned char)*p++;
    if (c == '\\') {
      char e = *p++;
      switch (e) {
        case '"': c = '"'; break;  case '\\': c = '\\'; break; case '/': c = '/'; break;
        case 'b': c = '\b'; break; case 'f': c = '\f'; break;  case 'n': c = '\n'; break;
        case 'r': c = '\r'; break; case 't': c = '\t'; break;
        case 'u': {
          unsigned u = 0;
          for (int k = 0; k < 4; k++) { char h = *p++; u <<= 4;
            if (h >= '0' && h <= '9') u |= (unsigned)(h - '0'); else if (h >= 'a' && h <= 'f') u |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') u |= (unsigned)(h - 'A' + 10); else return 0; }
          if (u < 0x80) c = u;
          else if (u < 0x800) { if (o + 2 >= n) return 0; out[o++] = (char)(0xC0 | (u >> 6)); c = 0x80 | (u & 0x3F); }
          else { if (o + 3 >= n) return 0; out[o++] = (char)(0xE0 | (u >> 12)); out[o++] = (char)(0x80 | ((u >> 6) & 0x3F)); c = 0x80 | (u & 0x3F); }
          break;
        }
        default: return 0;
      }
    }
    if (o + 1 >= n) return 0;
    out[o++] = (char)c;
  }
  if (*p != '"') return 0;
  out[o] = 0; *pp = p + 1;
  return 1;
}

/* Apply the {"k":..,"v":..} entries of `section` ("additions"/"updates"/"removals",
 * or NULL for a bare {"playerStateData":[...]}): op 0 = set, 1 = remove.
 * Returns -1 on a malformed list, else the number of entries. */
static int apply_section(State *st, const char *js, const char *section, int op) {
  const char *p = js;
  if (section) {
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\"", section);
    p = strstr(js, pat); if (!p) return 0;               /* absent section: nothing to do */
    p = skip_ws(p + strlen(pat)); if (*p++ != ':') return -1;
    p = skip_ws(p); if (*p != '{') return -1;
  }
  p = strstr(p, "\"playerStateData\""); if (!p) return -1;
  p = skip_ws(p + 17); if (*p++ != ':') return -1;
  p = skip_ws(p); if (*p++ != '[') return -1;
  static char k[1024], v[32768];
  int count = 0;
  for (;;) {
    p = skip_ws(p);
    if (*p == ',') { p++; continue; }
    if (*p == ']') break;
    if (*p++ != '{') return -1;
    int have_k = 0, have_v = 0;
    for (;;) {
      p = skip_ws(p);
      if (*p == ',') { p++; continue; }
      if (*p == '}') { p++; break; }
      char name[16];
      if (!json_str(&p, name, sizeof name)) return -1;
      p = skip_ws(p); if (*p++ != ':') return -1; p = skip_ws(p);
      if (!strcmp(name, "k"))      { if (!json_str(&p, k, sizeof k)) return -1; have_k = 1; }
      else if (!strcmp(name, "v")) { if (!json_str(&p, v, sizeof v)) return -1; have_v = 1; }
      else return -1;
    }
    if (!have_k) return -1;
    if (op == 1) state_del(st, k);
    else { if (!have_v) return -1; if (!state_set(st, k, v)) return -1; }
    count++;
  }
  return count;
}

/* Base + every transaction, as the game does. -1 on any malformed piece. */
static int replay(const Props *ps, State *st, int *tx_count) {
  memset(st, 0, sizeof *st);
  int ib = props_find(ps, "PSM_BasePlayerState"), ic = props_find(ps, "PSM_TransactionCount");
  if (ib < 0 || ic < 0) { report("this save has no PSM_BasePlayerState/PSM_TransactionCount -- not a format this editor knows"); return -1; }
  if (apply_section(st, ps->p[ib].val, NULL, 0) < 0) { report("PSM_BasePlayerState does not parse"); return -1; }
  char *e; long n = strtol(ps->p[ic].val, &e, 10);
  if (*e || n < 0 || n > 100000) { report("PSM_TransactionCount = \"%s\" is not a count", ps->p[ic].val); return -1; }
  for (long i = 0; i < n; i++) {
    char name[48]; snprintf(name, sizeof name, "PSM_TransactionCount_%ld", i);
    int it = props_find(ps, name);
    if (it < 0) { report("transaction %ld of %ld is missing", i, n); return -1; }
    const char *js = ps->p[it].val;
    if (apply_section(st, js, "additions", 0) < 0 || apply_section(st, js, "updates", 0) < 0 ||
        apply_section(st, js, "removals", 1) < 0) { report("transaction %ld does not parse", i); return -1; }
  }
  *tx_count = (int)n;
  return 0;
}

static long long state_ll(const State *s, const char *k, long long dflt) {
  const char *v = state_get(s, k); if (!v || !*v) return dflt;
  char *e; long long x = strtoll(v, &e, 10); return *e ? dflt : x;
}

/* ------------------------------------------------------------- AES ---- */
static int aes(int enc, uint8_t *dst, const uint8_t *src, size_t n) {
  if (n % 16) return 0;
  uint8_t key[16]; derive_key(key);
  Aes128CbcContext ctx;
  aes128CbcContextCreate(&ctx, key, SE_IV_STRING, enc);
  const size_t done = enc ? aes128CbcEncrypt(&ctx, dst, src, n) : aes128CbcDecrypt(&ctx, dst, src, n);
  return done == n;
}

/* -------------------------------------------------------------- IO ---- */
static uint8_t *read_all(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  if (sz < 0 || (unsigned long)sz > SE_MAX_FILE) { fclose(f); return NULL; }
  uint8_t *b = malloc((size_t)sz + 1);
  if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); b = NULL; }
  fclose(f);
  if (b) { b[sz] = 0; *n = (size_t)sz; }
  return b;
}
static int write_all(const char *path, const void *d, size_t n) {
  char tmp[320]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "wb"); if (!f) return 0;
  const int ok = fwrite(d, 1, n, f) == n;
  if (fclose(f) != 0 || !ok) { remove(tmp); return 0; }
  remove(path);                                           /* FAT: rename does not replace */
  return rename(tmp, path) == 0;
}
static int exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

/* --------------------------------------------------------- the edit ---- */
static const char *k_chars[8] = { NULL, "sonic", "tails", "knuckles", "amy", "sticks", "shadow", "vector" };
static const char *k_char_key[8] = { NULL, "Sonic", "Tails", "Knuckles", "Amy", "Sticks", "Shadow", "Vector" };  /* CharacterDefines.Type.ToString() */

/* Player level N (1..101) starts at this much XP -- ProgressLadderManager's
 * m_progressLadderItemList (level1 scene), each item's m_levelXpMin;
 * GetLevelForXp returns the level whose [min, max) holds the XP. */
static const int k_level_xp[101] = {
  0, 40, 120, 240, 390, 590, 795, 1010, 1240, 1490,
  1765, 2070, 2410, 2790, 3215, 3690, 4220, 4810, 5465, 6190,
  6990, 7870, 8835, 9890, 11040, 12290, 13645, 15110, 16690, 18390,
  20215, 22170, 24260, 26490, 28865, 31390, 34070, 36910, 39915, 43090,
  46440, 49970, 53685, 57590, 61690, 65990, 70495, 75210, 80140, 85290,
  90665, 96270, 102110, 108190, 114515, 121090, 127920, 135010, 142365, 149990,
  157890, 166070, 174535, 183290, 192340, 201690, 211345, 221310, 231590, 242190,
  253115, 264370, 275960, 287890, 300165, 312790, 325770, 339110, 352815, 366890,
  381340, 396170, 411385, 426990, 442990, 459390, 476195, 493410, 511040, 529090,
  547565, 566470, 585810, 605590, 625815, 646490, 667620, 689210, 711265, 733790,
  733890
};
#define SE_MAX_LEVEL        101
/* Character upgrade level: every character's ScoreMultiplier holds 15 values
 * (level1 scene: 1.0 1.1 1.2 1.3 2.5 ... 8.0), abilities at 4 and 9
 * (CharacterAbilityLevels.first/secondAbilityLockedLevel) -> 0..14. */
#define SE_CHAR_MAX_LEVEL   14
/* Durable sprite level: DurableSprite.Level Level1 = 0 .. Level11 = 10, stored as PackedData. */
#define SE_SPRITE_MAX_LEVEL 11
/* Presents: the prize-box products (game data), and the no-XP variants the level
 * ladder hands out. EventBox is left out: its contents come from server events. */
static const char *k_boxes[] = {
  "PaperBox", "PaperBox2", "PaperBox3", "PaperBox4", "BronzeBox", "SilverBox", "GoldBox",
  "PlatinumBox", "RubyBox", "EmeraldBox", "JackpotBox", "SpriteBox",
  "FTUEBox1", "FTUEBox2", "FTUESpriteBox", "FTUEVIPBox",
  "SilverBox0XP", "GoldBox0XP", "EmeraldBox0XP", "PlatinumBox0XP",
};
#define SE_MAX_PRESENTS 40
/* Sprites the game has (level5 scene: SpriteDatabase's DurableSprites and
 * ConsumableSprites children, the ones marked in the game). The name is the
 * GameObject's; the type is its group: 0 durable (levels 1-11), 1 consumable
 * (one use). A save holds them as SpriteInventoryState.Item[i].Sprite.Type /
 * .Sprite.Name / .PackedData -- the order the game writes a sprite drop. */
static const struct { const char *name; int type; } k_sprites[] = {
  { "IncreaseDashBarFillrate_Multiplier", 0 },
  { "IncreaseDashDuration_Extra", 0 },
  { "IncreaseEnerbeamChance_Extra", 0 },
  { "IncreaseEnerbeamDuration_Extra", 0 },
  { "IncreaseMagnetChance_Extra", 0 },
  { "IncreaseMagnetDuration_Extra", 0 },
  { "IncreaseShieldChance_Extra", 0 },
  { "IncreaseShieldDuration_Extra", 0 },
  { "IncreaseTwoTimesMultiplierChance_Extra", 0 },
  { "IncreaseTwoTimesMultiplierDuration_Extra", 0 },
  { "BankRingsDroppedLv1_Multiplier", 1 },
  { "BankRingsDroppedLv2_Multiplier", 1 },
  { "BankRingsDroppedLv3_Multiplier", 1 },
  { "BossDamageMultiplier_Multiplier", 1 },
  { "ComboWindowLv1_Extra", 1 },
  { "ComboWindowLv2_Extra", 1 },
  { "ComboWindowLv3_Extra", 1 },
  { "ExtraReviveLv1_Extra", 1 },
  { "ExtraReviveLv2_Extra", 1 },
  { "ExtraReviveLv3_Extra", 1 },
  { "IncreaseGlobalMultiplierLv1_Extra", 1 },
  { "IncreaseGlobalMultiplierLv2_Extra", 1 },
  { "IncreaseGlobalMultiplierLv3_Extra", 1 },
  { "IncreasePowerupChances_Max", 1 },
  { "RingsCollectedMultiplierLv1_Multiplier", 1 },
  { "RingsCollectedMultiplierLv2_Multiplier", 1 },
  { "RingsCollectedMultiplierLv3_Multiplier", 1 },
};

typedef struct {
  int rings_set, red_set, xp_set, dump;
  long long rings, red, xp;
  int level;                                              /* 0 = unset, else 1..101 */
  uint32_t unlock;                                        /* CharacterUnlocks bits to add */
  int char_level[8];                                      /* -1 = unset (index = character type) */
  int sprite_all_level;                                   /* 0 = unset, else 1..11 */
  struct { char name[48]; int level; } sprite_lv[32]; int n_sprite_lv;
  struct { char id[24]; int count; } presents[16]; int n_presents;
  int sprite_add[32]; int n_sprite_add;                   /* indexes into k_sprites */
} Edits;

static int parse_count(const char *s, int *count) {       /* "", "x3", "* 3", ", 3", "3" */
  while (*s == ' ' || *s == '\t' || *s == 'x' || *s == 'X' || *s == '*' || *s == ',') s++;
  if (!*s) { *count = 1; return 1; }
  char *e; long c = strtol(s, &e, 10);
  while (*e == ' ' || *e == '\t') e++;
  if (*e || c < 1 || c > 20) return 0;
  *count = (int)c; return 1;
}

static int parse_edits(const char *text, Edits *ed) {
  memset(ed, 0, sizeof *ed);
  for (int i = 0; i < 8; i++) ed->char_level[i] = -1;
  int n = 0;
  char line[512];
  for (const char *p = text; *p; ) {
    const char *nl = strchr(p, '\n'); size_t l = nl ? (size_t)(nl - p) : strlen(p);
    if (l >= sizeof line) l = sizeof line - 1;
    memcpy(line, p, l); line[l] = 0; p = nl ? nl + 1 : p + l;
    char *h = strchr(line, '#'); if (h) *h = 0;
    char *eq = strchr(line, '='); if (!eq) continue;
    *eq = 0;
    char *k = line, *v = eq + 1;
    while (isspace((unsigned char)*k)) k++;
    for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); ) *--e = 0;
    while (isspace((unsigned char)*v)) v++;
    for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); ) *--e = 0;
    for (char *c = k; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!*k || !*v) continue;
    char *e; long long x = strtoll(v, &e, 10);
    const int isnum = !*e && x >= 0 && x <= 2000000000LL;
    if (!strcmp(k, "rings"))          { if (isnum) { ed->rings = x; ed->rings_set = 1; n++; } else report("rings = \"%s\" is not a number 0..2000000000 -- skipped", v); }
    else if (!strcmp(k, "red_rings") || !strcmp(k, "red_star_rings")) { if (isnum) { ed->red = x; ed->red_set = 1; n++; } else report("red_star_rings = \"%s\" is not a number 0..2000000000 -- skipped", v); }
    else if (!strcmp(k, "xp"))        { if (isnum) { ed->xp = x; ed->xp_set = 1; n++; } else report("xp = \"%s\" is not a number 0..2000000000 -- skipped", v); }
    else if (!strcmp(k, "level"))     { if (isnum && x >= 1 && x <= SE_MAX_LEVEL) { ed->level = (int)x; n++; } else report("level = \"%s\" -- use 1 to %d; skipped", v, SE_MAX_LEVEL); }
    else if (!strcmp(k, "dump"))      { if (!strcasecmp(v, "yes") || !strcmp(v, "1") || !strcasecmp(v, "true")) { ed->dump = 1; n++; } }
    else if (!strcmp(k, "finish_character_purchase")) report("finish_character_purchase is no longer an option -- ignored");
    else if (!strcmp(k, "unlock")) {
      char list[256]; snprintf(list, sizeof list, "%s", v);
      for (char *tok = strtok(list, ", "); tok; tok = strtok(NULL, ", ")) {
        for (char *c = tok; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (!strcmp(tok, "all")) { ed->unlock |= 0x7Fu; continue; }
        int t = 0;
        for (int i = 1; i < 8; i++) if (!strcmp(tok, k_chars[i])) t = i;
        if (t) ed->unlock |= 1u << (t - 1);               /* CharacterUnLocks.GetIsUnlocked: bit (type-1) */
        else report("unlock: \"%s\" is not a character (sonic tails knuckles amy sticks shadow vector, or all)", tok);
      }
      if (ed->unlock) n++;
    } else if (!strncmp(k, "char.", 5)) {
      char who[24] = ""; const char *dot = strchr(k + 5, '.');
      if (!dot || strcmp(dot, ".level") || (size_t)(dot - (k + 5)) >= sizeof who) { report("%s: use char.<name>.level or char.all.level", k); continue; }
      memcpy(who, k + 5, (size_t)(dot - (k + 5))); who[dot - (k + 5)] = 0;
      if (!isnum || x > SE_CHAR_MAX_LEVEL) { report("%s = \"%s\" -- use 0 to %d; skipped", k, v, SE_CHAR_MAX_LEVEL); continue; }
      int t = 0;
      for (int i = 1; i < 8; i++) if (!strcmp(who, k_chars[i])) t = i;
      if (!strcmp(who, "all")) { for (int i = 1; i < 8; i++) if (ed->char_level[i] < 0) ed->char_level[i] = (int)x; n++; }
      else if (t) { ed->char_level[t] = (int)x; n++; }        /* a named line beats char.all */
      else report("%s: \"%s\" is not a character (sonic tails knuckles amy sticks shadow vector, or all)", k, who);
    } else if (!strcmp(k, "sprite.add")) {
      int found = -1;
      for (unsigned i = 0; i < sizeof k_sprites / sizeof *k_sprites; i++) if (!strcasecmp(v, k_sprites[i].name)) found = (int)i;
      if (found < 0) report("sprite.add: \"%s\" is not a sprite the game has (see save_edit.txt for the list) -- skipped", v);
      else if (ed->n_sprite_add < 32) { ed->sprite_add[ed->n_sprite_add++] = found; n++; }
    } else if (!strncmp(k, "sprite.", 7)) {
      const char *dot = strrchr(k + 7, '.');
      if (!dot || strcmp(dot, ".level") || dot == k + 7) { report("%s: use sprite.<name>.level or sprite.all.level", k); continue; }
      if (!isnum || x < 1 || x > SE_SPRITE_MAX_LEVEL) { report("%s = \"%s\" -- use 1 to %d; skipped", k, v, SE_SPRITE_MAX_LEVEL); continue; }
      const size_t wl = (size_t)(dot - (k + 7));
      if (wl == 3 && !strncmp(k + 7, "all", 3)) { ed->sprite_all_level = (int)x; n++; }
      else if (ed->n_sprite_lv < 32 && wl < sizeof ed->sprite_lv[0].name) {
        char *nm = ed->sprite_lv[ed->n_sprite_lv].name;
        memcpy(nm, k + 7, wl); nm[wl] = 0;                  /* the key was lower-cased: use the game's spelling */
        for (unsigned i = 0; i < sizeof k_sprites / sizeof *k_sprites; i++)
          if (!strcasecmp(nm, k_sprites[i].name)) snprintf(nm, sizeof ed->sprite_lv[0].name, "%s", k_sprites[i].name);
        ed->sprite_lv[ed->n_sprite_lv++].level = (int)x; n++;
      }
    } else if (!strcmp(k, "present")) {
      char id[64]; size_t il = strcspn(v, " \t,*xX");
      /* the box name ends at the first separator -- but a name may contain 'x' (JackpotBox0XP...) */
      const char *best = NULL; size_t bl = 0;
      for (unsigned b = 0; b < sizeof k_boxes / sizeof *k_boxes; b++) {
        const size_t L = strlen(k_boxes[b]);
        if (!strncasecmp(v, k_boxes[b], L) && (v[L] == 0 || strchr(" \t,*xX", v[L])) && L > bl &&
            !(L < strlen(v) && (v[L] == 'x' || v[L] == 'X') && isalpha((unsigned char)v[L + 1]))) { best = k_boxes[b]; bl = L; }
      }
      (void)il;
      int count = 1;
      if (!best) { snprintf(id, sizeof id, "%s", v); report("present: \"%s\" is not a box this editor knows (see save_edit.txt for the list) -- skipped", id); continue; }
      if (!parse_count(v + bl, &count)) { report("present = %s: the count must be 1 to 20 (\"%s x3\") -- skipped", best, best); continue; }
      if (ed->n_presents < 16) { snprintf(ed->presents[ed->n_presents].id, sizeof ed->presents[0].id, "%s", best); ed->presents[ed->n_presents++].count = count; n++; }
    } else report("unknown setting \"%s\" -- ignored", k);
  }
  return n;
}

/* ---- the changes one transaction will carry ---- */
typedef struct { char key[112]; char val[96]; int is_new; } Change;
typedef struct { Change *c; int n, cap; } Changes;

static Change *chg_slot(Changes *cs, const char *k) {
  for (int i = 0; i < cs->n; i++) if (!strcmp(cs->c[i].key, k)) return &cs->c[i];
  if (cs->n == cs->cap) { int nc = cs->cap ? cs->cap * 2 : 64; Change *nn = realloc(cs->c, (size_t)nc * sizeof *nn); if (!nn) return NULL; cs->c = nn; cs->cap = nc; }
  Change *c = &cs->c[cs->n++]; memset(c, 0, sizeof *c); snprintf(c->key, sizeof c->key, "%s", k);
  return c;
}
/* Record k = v unless the state already holds exactly that. */
static int chg_set(Changes *cs, const State *st, const char *k, const char *v) {
  const char *cur = state_get(st, k);
  if (cur && !strcmp(cur, v)) return 1;
  Change *c = chg_slot(cs, k);
  if (!c) return 0;
  snprintf(c->val, sizeof c->val, "%s", v);
  c->is_new = cur == NULL;
  return 1;
}
static int chg_set_ll(Changes *cs, const State *st, const char *k, long long v) {
  char b[32]; snprintf(b, sizeof b, "%lld", v); return chg_set(cs, st, k, b);
}

/* JSON string contents (our own values are plain ASCII; escape anyway). */
static void json_put(char **buf, size_t *len, size_t *cap, const char *s) {
  for (; *s; s++) {
    char e[8]; const char *w = e;
    if (*s == '"' || *s == '\\') { e[0] = '\\'; e[1] = *s; e[2] = 0; }
    else if ((unsigned char)*s < 0x20) snprintf(e, sizeof e, "\\u%04x", (unsigned char)*s);
    else { e[0] = *s; e[1] = 0; }
    const size_t wl = strlen(w);
    if (*len + wl + 1 > *cap) { size_t nc = *cap * 2 + wl + 64; char *nb = realloc(*buf, nc); if (!nb) return; *buf = nb; *cap = nc; }
    memcpy(*buf + *len, w, wl); *len += wl; (*buf)[*len] = 0;
  }
}
static void raw_put(char **buf, size_t *len, size_t *cap, const char *s) {
  const size_t wl = strlen(s);
  if (*len + wl + 1 > *cap) { size_t nc = *cap * 2 + wl + 64; char *nb = realloc(*buf, nc); if (!nb) return; *buf = nb; *cap = nc; }
  memcpy(*buf + *len, s, wl); *len += wl; (*buf)[*len] = 0;
}

/* The core, file-free so a PC test can drive it: `in` (the save file bytes)
 * + edits -> `*out` (new save file bytes). 1 = changed, 0 = nothing to do,
 * -1 = refused (the save is not touched). */
int sd_saveedit_transform(const uint8_t *in, size_t in_len, const char *edit_text,
                          uint8_t **out, size_t *out_len, char **dump_text) {
  *out = NULL; *out_len = 0; if (dump_text) *dump_text = NULL;
  Edits ed;
  if (!parse_edits(edit_text, &ed)) { report("save_edit.txt asks for nothing this editor does"); return 0; }

  if (in_len < 5 + 16) { report("the save is %zu bytes -- too short to be a save", in_len); return -1; }
  if (in[0] != SE_VERSION) { report("save version byte %u, expected %u -- refused", in[0], SE_VERSION); return -1; }
  const uint32_t ct_len = (uint32_t)in[1] | (uint32_t)in[2] << 8 | (uint32_t)in[3] << 16 | (uint32_t)in[4] << 24;
  if (ct_len != in_len - 5 || ct_len % 16) { report("save length field %u does not match the file (%zu) -- refused", ct_len, in_len - 5); return -1; }

  uint8_t *pt = malloc(ct_len + 1);
  if (!pt || !aes(0, pt, in + 5, ct_len)) { free(pt); report("decryption failed"); return -1; }
  size_t tlen = ct_len;
  while (tlen && pt[tlen - 1] == 0) tlen--;               /* PaddingMode.Zeros */
  if (ct_len - tlen >= 16 || !tlen || pt[tlen - 1] != '\n') { free(pt); report("decrypted save does not end as a property list -- refused"); return -1; }
  pt[tlen] = 0;

  Props ps; State st; int ntx = 0, rc = -1;
  Changes cs = { 0 };
  uint8_t *newct = NULL, *check = NULL; char *newpt = NULL;
  if (!props_parse(&ps, (const char *)pt, tlen)) { report("the decrypted property list does not parse -- refused"); goto done_nostate; }
  if (replay(&ps, &st, &ntx) < 0) goto done;
  {
    const long long x0 = state_ll(&st, "XP", 0); int lv = 1;
    while (lv < SE_MAX_LEVEL && x0 >= k_level_xp[lv]) lv++;
    char own[96] = ""; const long long u = state_ll(&st, "CharacterUnlocks", 1);
    for (int t = 1; t < 8; t++) if ((u >> (t - 1)) & 1) {
      char k[112]; snprintf(k, sizeof k, "CharacterStates.Item[%s].UpgradableLevel", k_char_key[t]);
      snprintf(own + strlen(own), sizeof own - strlen(own), "%s%s %lld", *own ? ", " : "", k_char_key[t], state_ll(&st, k, 0));
    }
    report("save read: level %d (XP %lld), rings %lld, red star rings %lld; characters (upgrade level): %s",
           lv, x0, state_ll(&st, "Rings", 0), state_ll(&st, "RedStarRingsFree", 0), own);
  }

  if (ed.dump && dump_text) {
    size_t cap = 64; for (int i = 0; i < st.n; i++) cap += strlen(st.a[i].k) + strlen(st.a[i].v) + 8;
    char *d = malloc(cap);
    if (d) { size_t o = 0; for (int i = 0; i < st.n; i++) o += (size_t)snprintf(d + o, cap - o, "%s = %s\n", st.a[i].k, st.a[i].v); *dump_text = d; }
  }

  long long rings = state_ll(&st, "Rings", 0), red = state_ll(&st, "RedStarRingsFree", 0);
  const long long rings0 = rings, red0 = red;
  if (ed.rings_set) rings = ed.rings;
  if (ed.red_set)   red = ed.red;
  if (ed.rings_set) chg_set_ll(&cs, &st, "Rings", rings);
  if (ed.red_set)   chg_set_ll(&cs, &st, "RedStarRingsFree", red);

  /* Player XP / level. level = N sets XP to where level N starts; an explicit
   * xp line wins. If XP goes DOWN, the ladder's "already shown" mark follows it. */
  long long xp = state_ll(&st, "XP", 0);
  const long long xp0 = xp;
  if (ed.level) xp = k_level_xp[ed.level - 1];
  if (ed.xp_set) xp = ed.xp;
  if (ed.level || ed.xp_set) {
    chg_set_ll(&cs, &st, "XP", xp);
    if (xp < xp0 && state_ll(&st, "ProgressLadderAnimatedXPAmount", 0) > xp)
      chg_set_ll(&cs, &st, "ProgressLadderAnimatedXPAmount", xp);
  }

  if (ed.unlock) chg_set_ll(&cs, &st, "CharacterUnlocks", state_ll(&st, "CharacterUnlocks", 1) | ed.unlock);

  /* Character upgrade levels: CharacterStates, keyed by CharacterDefines.Type.ToString(). */
  for (int t = 1; t < 8; t++) if (ed.char_level[t] >= 0) {
    char k[112]; snprintf(k, sizeof k, "CharacterStates.Item[%s].UpgradableLevel", k_char_key[t]);
    chg_set_ll(&cs, &st, k, ed.char_level[t]);
  }

  /* Sprites the save already holds: SpriteInventoryState.Item[i] -- only durable
   * ones level (PackedData = DurableSprite.Level, Level1 = 0). New sprites come from
   * SpriteBox presents, which the game fills with sprites itself. */
  int nsprites = 0;
  for (int i = 0; i < st.n; i++) {
    int idx; char tail[64];
    if (sscanf(st.a[i].k, "SpriteInventoryState.Item[%d].%63s", &idx, tail) != 2 || strcmp(tail, "PackedData")) continue;
    nsprites++;
    char pre[64]; snprintf(pre, sizeof pre, "SpriteInventoryState.Item[%d].", idx);
    const char *name = "?", *type = "?";
    for (int j = 0; j < st.n; j++) {
      if (strncmp(st.a[j].k, pre, strlen(pre))) continue;
      const char *m = st.a[j].k + strlen(pre);
      if (strstr(m, "Name")) name = st.a[j].v;
      else if (strstr(m, "Type")) type = st.a[j].v;
    }
    const int durable = !strcmp(type, "0") || !strcasecmp(type, "Durable");
    const long long cur = state_ll(&st, st.a[i].k, 0);
    report("sprite %d: %s (%s), level %lld", idx, name, durable ? "durable" : "consumable", durable ? cur + 1 : 0);
    int want = durable && ed.sprite_all_level ? ed.sprite_all_level : 0;
    for (int k = 0; k < ed.n_sprite_lv; k++) if (!strcasecmp(ed.sprite_lv[k].name, name)) {
      if (durable) want = ed.sprite_lv[k].level;
      else report("sprite.%s.level: %s is a consumable sprite -- consumables have no level", ed.sprite_lv[k].name, name);
    }
    if (want) chg_set_ll(&cs, &st, st.a[i].k, want - 1);
  }
  /* New sprites, written as the game writes a drop; never past the inventory's size. */
  if (ed.n_sprite_add) {
    int nitems = 0;
    for (int i = 0; i < st.n; i++) { int idx; if (sscanf(st.a[i].k, "SpriteInventoryState.Item[%d].", &idx) == 1 && idx + 1 > nitems) nitems = idx + 1; }
    const long long cap = state_ll(&st, "NumUnlockedSpriteInventorySlots", 16);
    for (int a = 0; a < ed.n_sprite_add; a++) {
      const int si = ed.sprite_add[a];
      if (nitems >= cap) { report("sprite.add %s: the sprite inventory is full (%lld) -- skipped", k_sprites[si].name, cap); continue; }
      int lvl = 1;
      if (k_sprites[si].type == 0) {
        if (ed.sprite_all_level) lvl = ed.sprite_all_level;
        for (int k = 0; k < ed.n_sprite_lv; k++) if (!strcasecmp(ed.sprite_lv[k].name, k_sprites[si].name)) lvl = ed.sprite_lv[k].level;
      }
      char key[112], val[16];
      snprintf(key, sizeof key, "SpriteInventoryState.Item[%d].Sprite.Type", nitems); snprintf(val, sizeof val, "%d", k_sprites[si].type); chg_set(&cs, &st, key, val);
      snprintf(key, sizeof key, "SpriteInventoryState.Item[%d].Sprite.Name", nitems); chg_set(&cs, &st, key, k_sprites[si].name);
      snprintf(key, sizeof key, "SpriteInventoryState.Item[%d].PackedData", nitems); snprintf(val, sizeof val, "%d", k_sprites[si].type == 0 ? lvl - 1 : 0); chg_set(&cs, &st, key, val);
      if (k_sprites[si].type == 0) report("sprite added: %s (durable, level %d)", k_sprites[si].name, lvl);
      else report("sprite added: %s (consumable)", k_sprites[si].name);
      nitems++;
    }
  }
  if ((ed.sprite_all_level || ed.n_sprite_lv) && !nsprites && !ed.n_sprite_add)
    report("sprite levels: this save holds no sprites yet -- get some first (present = SpriteBox)");
  for (int k = 0; k < ed.n_sprite_lv; k++) {
    int found = 0;
    for (int a = 0; a < ed.n_sprite_add; a++) if (!strcasecmp(k_sprites[ed.sprite_add[a]].name, ed.sprite_lv[k].name)) found = 1;
    for (int i = 0; i < st.n; i++) if (strstr(st.a[i].k, "SpriteInventoryState.Item[") && !strcasecmp(st.a[i].v, ed.sprite_lv[k].name)) found = 1;
    if (!found) report("sprite.%s.level: you have no sprite of that name -- skipped (sprite.add gives you one)", ed.sprite_lv[k].name);
  }

  /* Presents: new LocalInboxItems entries, every field as the game writes a mission
   * prize (TitleId, Reason, ... copied from the game's own entries); MessageID is the
   * next local id, -NextInboxId, as the game numbers them. */
  int ninbox = 0;
  for (int i = 0; i < st.n; i++) { int idx; if (sscanf(st.a[i].k, "LocalInboxItems.Item[%d].", &idx) == 1 && idx + 1 > ninbox) ninbox = idx + 1; }
  long long next_id = state_ll(&st, "NextInboxId", 0);
  int added = 0;
  for (int p = 0; p < ed.n_presents; p++)
    for (int c = 0; c < ed.presents[p].count && added < SE_MAX_PRESENTS; c++, added++) {
      const int it = ninbox + added;
      char k[112], v[96];
      #define INBOX(f, val) do { snprintf(k, sizeof k, "LocalInboxItems.Item[%d].%s", it, f); chg_set(&cs, &st, k, val); } while (0)
      INBOX("TitleId", "-520520811");
      INBOX("DescriptionId", "0");
      INBOX("DescriptionString", "A present from the save editor");
      INBOX("ProductId", ed.presents[p].id);
      INBOX("RewardInteger", "1");
      snprintf(v, sizeof v, "%lld", -(next_id + added)); INBOX("MessageID", v);
      INBOX("MenuPage", "0");
      INBOX("MessageType", "0");
      INBOX("NewRank", "0");
      INBOX("Reason", "-1402310286");
      #undef INBOX
      report("present added: %s", ed.presents[p].id);
    }
  if (added) chg_set_ll(&cs, &st, "NextInboxId", next_id + added);

  if (!cs.n) { report("every requested value is already so -- the save is unchanged"); rc = 0; goto done; }

  /* The new transaction, shaped as the game writes them. */
  size_t ucap = 256, acap = 256, ul = 0, al = 0;
  char *upd = calloc(1, ucap), *add = calloc(1, acap);
  if (!upd || !add) { free(upd); free(add); report("out of memory"); goto done; }
  for (int i = 0; i < cs.n; i++) {
    char **b = cs.c[i].is_new ? &add : &upd; size_t *bl2 = cs.c[i].is_new ? &al : &ul; size_t *bc = cs.c[i].is_new ? &acap : &ucap;
    raw_put(b, bl2, bc, *bl2 ? ", {\"k\":\"" : "{\"k\":\""); json_put(b, bl2, bc, cs.c[i].key);
    raw_put(b, bl2, bc, "\",\"v\":\"");                 json_put(b, bl2, bc, cs.c[i].val); raw_put(b, bl2, bc, "\"}");
    const char *old = state_get(&st, cs.c[i].key);
    if (strncmp(cs.c[i].key, "LocalInboxItems.", 16)) report("%s: %s -> %s", cs.c[i].key, old ? old : "(none)", cs.c[i].val);
  }
  const size_t jcap = ul + al + 512;
  char *js = malloc(jcap);
  if (!js) { free(upd); free(add); report("out of memory"); goto done; }
  snprintf(js, jcap,
           "{\"redStarRingsPurchasedDelta\":0,\"removals\":{\"playerStateData\":[]},\"goldRingsDelta\":%lld,"
           "\"conflictLocalBaseVersion\":-1,\"redStarRingsFreeDelta\":%lld,\"checksum\":0,"
           "\"updates\":{\"playerStateData\":[%s]},\"additions\":{\"playerStateData\":[%s]},"
           "\"transactionName\":\"Save editor (Switch port)\"}", rings - rings0, red - red0, upd, add);
  free(upd); free(add);

  char name[48]; snprintf(name, sizeof name, "PSM_TransactionCount_%d", ntx);
  if (props_find(&ps, name) >= 0) { report("%s already exists -- refused", name); goto done; }
  int at = props_find(&ps, "PSM_TransactionCount") + 1;
  if (ntx > 0) { char last[48]; snprintf(last, sizeof last, "PSM_TransactionCount_%d", ntx - 1); at = props_find(&ps, last) + 1; }
  const int ins_ok = props_insert(&ps, at, sd_saveedit_crc(name), js);
  free(js);
  if (!ins_ok) { report("out of memory"); goto done; }
  char cnt[16]; snprintf(cnt, sizeof cnt, "%d", ntx + 1);
  const int ic = props_find(&ps, "PSM_TransactionCount");
  free(ps.p[ic].val); ps.p[ic].val = strdup(cnt);
  if (!ps.p[ic].val) { report("out of memory"); goto done; }

  size_t nlen = 0;
  newpt = props_serialize(&ps, &nlen);
  if (!newpt) { report("out of memory"); goto done; }
  const size_t padded = (nlen + 15) & ~(size_t)15;
  memset(newpt + nlen, 0, padded - nlen);

  { /* Self-check: re-parse and replay what is about to be written. */
    Props vps; State vst; int vtx = 0; int ok = props_parse(&vps, newpt, nlen);
    if (ok) { ok = replay(&vps, &vst, &vtx) == 0 && vtx == ntx + 1;
      for (int i = 0; ok && i < cs.n; i++) { const char *v = state_get(&vst, cs.c[i].key); ok = v && !strcmp(v, cs.c[i].val); }
      state_free(&vst); }
    props_free(&vps);
    if (!ok) { report("self-check failed: the edited save does not replay to the requested values -- refused"); goto done; }
  }

  newct = malloc(padded); check = malloc(padded);
  if (!newct || !check || !aes(1, newct, (const uint8_t *)newpt, padded) || !aes(0, check, newct, padded) ||
      memcmp(check, newpt, padded)) { report("encryption round trip failed -- refused"); goto done; }

  *out_len = 5 + padded; *out = malloc(*out_len);
  if (!*out) { report("out of memory"); goto done; }
  (*out)[0] = in[0];
  (*out)[1] = (uint8_t)padded; (*out)[2] = (uint8_t)(padded >> 8); (*out)[3] = (uint8_t)(padded >> 16); (*out)[4] = (uint8_t)(padded >> 24);
  memcpy(*out + 5, newct, padded);
  report("new transaction %s written (%d value(s)); the save now has %d properties", name, cs.n, ps.n);
  rc = 1;
done:
  state_free(&st);
  free(cs.c);
done_nostate:
  props_free(&ps); free(pt); free(newpt); free(newct); free(check);
  return rc;
}

/* ------------------------------------------------------------ boot ---- */
/* ---- save_edit.txt: every option, in sections --------------------------
 * The file is written once, so an older file never showed options added later.
 * Each section names the setting that marks it; a file whose last line lacks
 * SE_TEMPLATE_MARK gets the sections it does not mention appended (sonicdash_nx
 * did the same for its roster). The player's own lines are never touched. */
#define SE_TEMPLATE_MARK "# [save_edit.txt option list v3]"

static void put_section(FILE *f, int which) {
  switch (which) {
  case 0:
    fputs("# --- currencies ----------------------------------------------------------\n"
          "#rings = 50000\n"
          "#red_star_rings = 500\n", f);
    break;
  case 1:
    fputs("# --- player level --------------------------------------------------------\n"
          "# level sets your XP to where that level starts (1 to 101; 101 is the top).\n"
          "# Going up, the progress ladder hands out the levels' rewards (score\n"
          "# multiplier, prize boxes) the next time it plays, as if you had earned it.\n"
          "# xp sets the exact amount instead, and wins if both are given.\n"
          "#level = 20\n"
          "#xp = 5000\n", f);
    break;
  case 2:
    fputs("# --- characters ----------------------------------------------------------\n"
          "# unlock adds characters to the ones you own -- several separated by\n"
          "# commas, or all:\n"
          "#unlock = all\n", f);
    for (int t = 1; t < 8; t++) fprintf(f, "#unlock = %s\n", k_chars[t]);
    fputs("# A character's upgrade level: 0 (not upgraded) to 14 (fully upgraded). It\n"
          "# sets their score multiplier -- x1.0 at 0, then 1.1 1.2 1.3 2.5 2.7 2.9 3.1\n"
          "# 3.3 5.0 5.3 5.6 5.9 6.2 and x8.0 at 14; the first ability arrives at\n"
          "# level 4 and the second at 9. char.all sets every character; a line\n"
          "# naming one character beats it.\n"
          "#char.all.level = 14\n", f);
    for (int t = 1; t < 8; t++) fprintf(f, "#char.%s.level = 14\n", k_chars[t]);
    break;
  case 3:
    fputs("# --- presents ------------------------------------------------------------\n"
          "# Puts a present in your inbox; open it in the game as usual (no connection\n"
          "# needed). Add \"x3\" (up to x20) for several. Each present line is used once,\n"
          "# then commented out for you; several present lines can be used at once.\n"
          "# The ...0XP boxes are the level ladder's versions, which give no XP.\n"
          "# SpriteBox and FTUESpriteBox hold a sprite.\n", f);
    for (unsigned i = 0; i < sizeof k_boxes / sizeof *k_boxes; i++) fprintf(f, "#present = %s\n", k_boxes[i]);
    break;
  case 4:
    fputs("# --- sprites -------------------------------------------------------------\n"
          "# sprite.add gives you a sprite (one per line; used once, then commented\n"
          "# out for you; your inventory holds 16). Durable sprites stay and level up\n"
          "# (1 to 11); consumable ones are used up in a run.\n"
          "# durable:\n", f);
    for (unsigned i = 0; i < sizeof k_sprites / sizeof *k_sprites; i++)
      if (k_sprites[i].type == 0) fprintf(f, "#sprite.add = %s\n", k_sprites[i].name);
    fputs("# consumable:\n", f);
    for (unsigned i = 0; i < sizeof k_sprites / sizeof *k_sprites; i++)
      if (k_sprites[i].type == 1) fprintf(f, "#sprite.add = %s\n", k_sprites[i].name);
    fputs("# Levels for durable sprites you have (or add in the same launch): 1 to 11.\n"
          "# save_edit_result.txt lists yours after any launch that applied a setting.\n"
          "#sprite.all.level = 11\n", f);
    for (unsigned i = 0; i < sizeof k_sprites / sizeof *k_sprites; i++)
      if (k_sprites[i].type == 0) fprintf(f, "#sprite.%s.level = 11\n", k_sprites[i].name);
    break;
  case 5:
    fputs("# --- information ---------------------------------------------------------\n"
          "# Writes your whole player state, readable, to save_dump.txt at each launch.\n"
          "#dump = yes\n", f);
    break;
  }
}
/* The setting that shows a file already has section i. */
static const char *const k_section_key[6] = { "rings", "level", "char.", "present", "sprite.", "dump" };

/* Does the file mention setting `key` at the start of a line ("key" or "#key")? */
static int mentions(const char *text, const char *key) {
  const size_t kl = strlen(key);
  for (const char *p = text; *p; ) {
    const char *q = p; while (*q == ' ' || *q == '\t' || *q == '#') q++;
    if (!strncasecmp(q, key, kl) && (key[kl - 1] == '.' || q[kl] == ' ' || q[kl] == '\t' || q[kl] == '=')) return 1;
    const char *nl = strchr(p, '\n'); if (!nl) break; p = nl + 1;
  }
  return 0;
}

static void write_template(const char *path) {
  FILE *f = fopen(path, "w");
  if (!f) { debugPrintf("[saveedit] could not write %s\n", path); return; }
  fputs(
"# save_edit.txt -- Sonic Dash 2: Sonic Boom save editing (sonicboomdash).\n"
"#\n"
"# Every line is commented out. Remove the '#' from one and give it a value; it\n"
"# is applied to your save at EVERY launch, for as long as the line stays\n"
"# uncommented -- \"rings = 50000\" puts you back at 50000 rings each launch.\n"
"# Comment it out again once the change has taken. (present and sprite.add\n"
"# lines are the exception: each is used once, then commented out for you.)\n"
"#\n"
"# The save (the file named save) is encrypted, so it cannot be edited by hand.\n"
"# Editing here is safe: each change is written the way the game records one --\n"
"# a new entry in its own list of changes -- and checked before anything is\n"
"# written. The untouched original is kept once as save.original, and the save\n"
"# as it was before each edit as save.before-edit.\n"
"# What each launch did is written to save_edit_result.txt.\n"
"# Quit the game fully before editing: it saves on the way out.\n", f);
  for (int i = 0; i < 6; i++) { fputs("\n", f); put_section(f, i); }
  fputs("\n" SE_TEMPLATE_MARK "\n", f);
  fclose(f);
  debugPrintf("[saveedit] wrote %s (every setting commented out)\n", path);
}

/* An older save_edit.txt: append the sections it does not mention. */
static void upgrade_template(const char *path, const char *text) {
  if (strstr(text, SE_TEMPLATE_MARK)) return;
  FILE *f = fopen(path, "a");
  if (!f) return;
  fputs("\n\n# ==== options added by a newer build (your lines above are unchanged) ====\n", f);
  int added = 0;
  for (int i = 0; i < 6; i++) if (!mentions(text, k_section_key[i])) { fputs("\n", f); put_section(f, i); added++; }
  if (mentions(text, "finish_character_purchase"))
    fputs("\n# finish_character_purchase (above) is no longer an option.\n", f);
  fputs("\n" SE_TEMPLATE_MARK "\n", f);
  fclose(f);
  debugPrintf("[saveedit] save_edit.txt is from an older build: %d section(s) of new options appended\n", added);
}

/* After presents were added, comment their lines out: a present line is a
 * one-off, unlike the settings, which are re-applied at every launch. */
static void comment_out_presents(const char *path, const char *text) {
  size_t n = strlen(text), cap = n + 256, o = 0;
  char *out = malloc(cap); if (!out) return;
  for (const char *p = text; *p; ) {
    const char *nl = strchr(p, '\n'); size_t l = nl ? (size_t)(nl - p + 1) : strlen(p);
    const char *q = p; while (q < p + l && (*q == ' ' || *q == '\t')) q++;
    int is_present = 0;
    for (int w = 0; w < 2 && !is_present; w++) {
      const char *kw = w ? "sprite.add" : "present"; const size_t kl = strlen(kw);
      if (!strncasecmp(q, kw, kl)) { const char *r = q + kl; while (*r == ' ' || *r == '\t') r++; is_present = *r == '='; }
    }
    if (is_present) {
      if (o + l + 48 > cap) { cap = (o + l + 48) * 2; char *nb = realloc(out, cap); if (!nb) { free(out); return; } out = nb; }
      out[o++] = '#';
      size_t body = l - (nl ? 1 : 0); while (body && (p[body - 1] == '\r')) body--;
      memcpy(out + o, p, body); o += body;
      const char *tag = "   # added at the last launch\n"; memcpy(out + o, tag, strlen(tag)); o += strlen(tag);
    } else {
      if (o + l + 1 > cap) { cap = (o + l + 1) * 2; char *nb = realloc(out, cap); if (!nb) { free(out); return; } out = nb; }
      memcpy(out + o, p, l); o += l;
    }
    p += l;
  }
  if (!write_all(path, out, o)) debugPrintf("[saveedit] could not comment out the present lines in %s\n", path);
  free(out);
}

/* Is any setting uncommented? (Nothing to do, and nothing to read, if not.) */
static int has_settings(const char *t) {
  for (const char *p = t; *p; ) {
    while (*p == ' ' || *p == '\t') p++;
    const char *nl = strchr(p, '\n'), *e = nl ? nl : p + strlen(p);
    if (p < e && *p != '#' && *p != '\r' && memchr(p, '=', (size_t)(e - p))) return 1;
    if (!nl) break;
    p = nl + 1;
  }
  return 0;
}

int sd_saveedit_apply(void) {
  char pedit[300], psave[300], pbak[300];
  snprintf(pedit, sizeof pedit, "%s/save_edit.txt", GAME_HOME);
  size_t elen = 0; uint8_t *etext = read_all(pedit, &elen);
  if (!etext) { write_template(pedit); return 0; }         /* first launch: the template */
  upgrade_template(pedit, (const char *)etext);            /* an older file: add the options it lacks */
  if (!has_settings((const char *)etext)) { free(etext); return 0; }   /* all commented: the normal case */
  s_rlen = 0; s_report[0] = 0;
  debugPrintf("[saveedit] save_edit.txt has settings -- applying\n");

  snprintf(psave, sizeof psave, "%s/save", GAME_HOME);
  size_t slen = 0; uint8_t *save = read_all(psave, &slen);
  int rc = -1;
  if (!save) report("no save file yet (%s) -- play until the game has saved once, then edit", psave);
  else {
    uint8_t *out = NULL; size_t olen = 0; char *dump = NULL;
    rc = sd_saveedit_transform(save, slen, (const char *)etext, &out, &olen, &dump);
    if (dump) {
      char pd[300]; snprintf(pd, sizeof pd, "%s/save_dump.txt", GAME_HOME);
      if (write_all(pd, dump, strlen(dump))) report("current state written to save_dump.txt");
      free(dump);
    }
    if (rc == 1) {
      snprintf(pbak, sizeof pbak, "%s/save.original", GAME_HOME);
      if (!exists(pbak)) write_all(pbak, save, slen);
      snprintf(pbak, sizeof pbak, "%s/save.before-edit", GAME_HOME);
      char pback[300]; snprintf(pback, sizeof pback, "%s/save-backup", GAME_HOME);
      if (!write_all(pbak, save, slen)) { report("could not write save.before-edit -- nothing changed"); rc = -1; }
      else if (!write_all(psave, out, olen)) { report("could not write the new save -- the old one is in save.before-edit"); rc = -1; }
      else if (!write_all(pback, out, olen)) report("save written; save-backup could not be updated (the game rewrites it on its next save)");
      else report("save and save-backup updated; the previous save is save.before-edit");
      if (rc == 1 && (strstr(s_report, "present added:") || strstr(s_report, "sprite added:")))
        comment_out_presents(pedit, (const char *)etext);
    }
    free(out); free(save);
  }

#ifdef __SWITCH__
  if (rc == 1) fsdevCommitDevice("sdmc");
#endif
  /* save_edit.txt stays as it is (its settings apply at every launch); what
   * this launch did with them goes next to it. */
  char pres[300]; snprintf(pres, sizeof pres, "%s/save_edit_result.txt", GAME_HOME);
  FILE *f = fopen(pres, "wb");
  if (f) {
    fprintf(f, "# What the last launch did with save_edit.txt: %s\n%s",
            rc == 1 ? "CHANGED THE SAVE" : rc == 0 ? "no change needed" : "REFUSED (the save was not touched)", s_report);
    fclose(f);
  }
  free(etext);
  return rc;
}
