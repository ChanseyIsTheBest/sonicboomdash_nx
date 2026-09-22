/* sd_jnilog.c -- extensive JNI logging: every Java call the game makes, with
 * its arguments and the answer this port gave, one line each:
 *
 *   [jnicall] #1234 T1 com/foo/Bar.baz(Ljava/lang/String;I)Z ("abc", 7) -> 1  [route]
 *
 * route = who answered, by the method's class: game (sd_jni.c's claims for the
 * game's own Java classes), sdk-dormant (the inert-SDK policy), unity
 * (unity_jni.c), text/movie, or core -- jni_fake.c's built-in answers: some are
 * deliberate special cases (String.length, getClass, Runnable posting), the rest
 * name-based defaults. A wrong answer on a "core" line is the first place to look.
 *
 * The normal JNI log shows each method's first three answers only, so a method
 * first used at boot is invisible later. Here every call is logged, throttled
 * so per-frame calls cannot drown the log or slow the game:
 *   - the first 64 calls of each method in full, then every 256th with its count;
 *   - inside a tap window (sd_jni_tap_window, opened by every tap) all of them;
 *   - at most 1500 lines a second (a note says how many were held back).
 * Field reads are logged the same way ([jnifield]).
 *
 * config.txt  jni_log = extensive (default) | normal | off
 * Reading an answer never crashes the game: every pointer's memory is asked
 * of the kernel before its tag is read.
 * ------------------------------------------------------------------------- */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "sd_config.h"
#include "sd_jnilog.h"
#include "util.h"

struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };   /* = jni_fake.c's FakeID */
_Static_assert(sizeof(struct FakeID) == 324, "mirror of jni_fake.c's FakeID layout");

extern const char *jni_string_utf(void *jstr);
const char *sd_jni_eff_cls(const void *id, void *recv);
int sd_jni_tap_active(void);

#define T_CLASS  0x434c5331u
#define T_OBJECT 0x4f424a31u
#define T_STRING 0x53545231u
#define T_OBJARR 0x4f415231u
#define T_PRIARR 0x50415231u
#define T_ID     0x4d494431u
#define T_UHAND  0x554a4831u

static int readable(const void *p, size_t n) {
  MemoryInfo mi; u32 pi;
  if (!p || ((uintptr_t)p & 3)) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)p))) return 0;
  return mi.type != MemType_Unmapped && (mi.perm & Perm_R) && (uintptr_t)p + n <= mi.addr + mi.size;
}

static size_t put_obj(char *b, size_t n, const void *p) {
  if (!p) return (size_t)snprintf(b, n, "null");
  if (!readable(p, 8)) return (size_t)snprintf(b, n, "<%p>", p);
  const uint32_t tag = *(const uint32_t *)p;
  switch (tag) {
    case T_STRING: {
      const char *u = jni_string_utf((void *)p);
      char q[132]; size_t k = 0;
      for (const char *c = u; *c && k < sizeof q - 4; c++) q[k++] = (*c == '\n' || *c == '\r') ? '|' : *c;
      q[k] = 0;
      return (size_t)snprintf(b, n, "\"%s\"%s", q, strlen(u) > k ? "..." : "");
    }
    case T_CLASS:  return (size_t)snprintf(b, n, "<%.90s>", (const char *)p + 4);
    case T_OBJECT: return (size_t)snprintf(b, n, "<obj %.90s>", (const char *)p + 4);
    case T_OBJARR: return (size_t)snprintf(b, n, "<Object[%d]>", *(const int *)((const char *)p + 4));
    case T_PRIARR: return (size_t)snprintf(b, n, "<prim[%d]>", *(const int *)((const char *)p + 4));
    case T_UHAND:  return (size_t)snprintf(b, n, "<handle kind %d>", *(const int *)((const char *)p + 4));
    case T_ID: {
      const struct FakeID *m = p;
      return (size_t)snprintf(b, n, "<member %.50s.%.40s>", m->cls, m->name);
    }
  }
  return (size_t)snprintf(b, n, "<%p>", p);
}

/* The arguments, decoded by the method's own signature. `va` is a COPY. */
static void put_args(char *b, size_t n, const char *sig, va_list va) {
  size_t o = 0; b[0] = 0;
  const char *p = sig;
  if (*p++ != '(') return;
  int first = 1;
  while (*p && *p != ')' && o + 24 < n) {
    if (!first) o += (size_t)snprintf(b + o, n - o, ", ");
    first = 0;
    const char t = *p;
    if (t == 'L' || t == '[') {
      while (*p == '[') p++;
      if (*p == 'L') { const char *e = strchr(p, ';'); p = e ? e + 1 : p + strlen(p); } else if (*p) p++;
      o += put_obj(b + o, n - o, va_arg(va, void *));
    } else if (t == 'J') { o += (size_t)snprintf(b + o, n - o, "%lld", va_arg(va, long long)); p++; }
    else if (t == 'F' || t == 'D') { o += (size_t)snprintf(b + o, n - o, "%g", va_arg(va, double)); p++; }
    else if (t == 'Z') { o += (size_t)snprintf(b + o, n - o, "%s", va_arg(va, int) ? "true" : "false"); p++; }
    else { o += (size_t)snprintf(b + o, n - o, "%d", va_arg(va, int)); p++; }
    if (o >= n) { o = n - 1; break; }
  }
}

/* ---------------------------------------------------------- throttling ---- */
static uint32_t method_count(const char *c, const char *m) {
  static struct { uint32_t h, n; } t[16384];
  static int used; static Mutex lk;
  uint32_t h = 2166136261u;
  for (const char *p = c; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  h = (h ^ '.') * 16777619u;
  for (const char *p = m; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  if (!h) h = 1;
  uint32_t n = 0;
  mutexLock(&lk);
  for (uint32_t i = 0; i < 16384; i++) {
    uint32_t j = (h + i) & 16383;
    if (t[j].h == h) { n = ++t[j].n; break; }
    if (!t[j].h) { if (used < 12000) { t[j].h = h; t[j].n = 1; used++; n = 1; } else n = 0xFFFFFFFFu; break; }
  }
  mutexUnlock(&lk);
  return n;
}
static int rate_ok(void) {                 /* <= 1500 lines a second */
  static uint64_t sec; static unsigned lines, held;
  const uint64_t now = armTicksToNs(armGetSystemTick()) / 1000000000ull;
  if (now != sec) {
    if (held) debugPrintf("[jnicall] (%u lines held back in the last second: rate limit)\n", held);
    sec = now; lines = 0; held = 0;
  }
  if (lines < 1500) { lines++; return 1; }
  held++; return 0;
}
static unsigned thread_no(void) {
  static __thread unsigned me; static unsigned next;
  if (!me) me = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
  return me;
}
/* 1 = full line, 2 = periodic summary line, 0 = nothing */
static int admit(const char *c, const char *m, uint32_t *count) {
  if (sd_cfg_jni_log < 2) return 0;
  *count = method_count(c, m);
  const int tap = sd_jni_tap_active();
  if (!tap && *count > 64 && (*count % 256)) return 0;
  if (!tap && !rate_ok()) return 0;
  return (tap || *count <= 64) ? 1 : 2;
}

static const char *const k_route[] = { "game", "sdk-dormant", "unity", "text/movie", "core" };

void sd_jnilog_call(char kind, const void *id_, void *recv, int route, va_list args,
                    uint64_t iret, double fret) {
  const struct FakeID *id = id_;
  if (!id) return;
  static volatile uint32_t seq;
  const uint32_t my = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
  const char *c = sd_jni_eff_cls(id, recv);
  uint32_t count = 0;
  const int a = admit(c, id->name, &count);
  if (!a) return;
  char ret[160];
  if (kind == 'o')      put_obj(ret, sizeof ret, (const void *)(uintptr_t)iret);
  else if (kind == 'f') snprintf(ret, sizeof ret, "%g", fret);
  else if (kind == 'v') snprintf(ret, sizeof ret, "(void)");
  else                  snprintf(ret, sizeof ret, "%lld", (long long)iret);
  const char *rt = route >= 0 && route < 5 ? k_route[route] : "?";
  if (a == 2) {
    debugPrintf("[jnicall] #%u T%u %s.%s: call %u (every 256th logged) -> %s  [%s]\n",
                my, thread_no(), c, id->name, count, ret, rt);
    return;
  }
  char argv[420];
  put_args(argv, sizeof argv, id->sig, args);
  char recvs[112] = "";
  if (recv) put_obj(recvs, sizeof recvs, recv);
  debugPrintf("[jnicall] #%u T%u %s.%s%s (%s) -> %s  [%s]%s%s\n", my, thread_no(), c, id->name, id->sig,
              argv, ret, rt, recv ? "  on " : "", recvs);
}

void sd_jnilog_field(char kind, const void *id_, int is_static, uint64_t iv, double fv, const void *ov) {
  const struct FakeID *id = id_;
  if (!id) return;
  uint32_t count = 0;
  const int a = admit(id->cls, id->name, &count);
  if (!a) return;
  char val[160];
  if (kind == 'o')      put_obj(val, sizeof val, ov);
  else if (kind == 'f') snprintf(val, sizeof val, "%g", fv);
  else                  snprintf(val, sizeof val, "%lld", (long long)iv);
  (void)is_static;                     /* the env table routes static and instance reads alike */
  debugPrintf("[jnifield] T%u %s.%s %s = %s%s\n", thread_no(), id->cls, id->name,
              id->sig, val, a == 2 ? "  (every 256th read logged)" : "");
}
