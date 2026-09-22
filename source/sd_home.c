/* sd_home.c -- the game folder, found at run time.
 *
 * The folder used to be a compile-time constant (sdmc:/switch/sonicboomdash),
 * so a copy in any other folder -- sonicboomdash_nx, say -- looked for its
 * libraries in the wrong place and stopped at "Missing library: libmain.so".
 * The loader now uses whatever folder the .nro was started from:
 *
 *   1. argv[0] -- hbmenu / hbloader pass the .nro's own path, e.g.
 *      "sdmc:/switch/sonicboomdash_nx/sonicboomdash.nro";
 *   2. the current directory -- some launchers chdir to the .nro's folder;
 *   3. the built-in default, sdmc:/switch/sonicboomdash.
 *
 * A candidate is accepted only if libunity.so is actually there. If none has
 * it, the .nro's own folder is kept anyway, so the "missing library" error
 * names the folder the player actually used. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "sd_home.h"

static char s_home[256] = "sdmc:/switch/" GAME_FOLDER_DEFAULT;
static char s_how[64]   = "built-in default";

const char *sd_home(void)     { return s_home; }
const char *sd_home_how(void) { return s_how; }

const char *sd_home_bare(void) {
  const char *c = strchr(s_home, ':');
  return (c && (c[1] == '/' || c[1] == 0)) ? c + 1 : s_home;
}

const char *sd_log_path(void) {
  static char p[288];
  snprintf(p, sizeof p, "%s/debug.log", s_home);
  return p;
}

/* dir of `path`, with a device prefix ("sdmc:") added when missing and no
 * trailing '/'. 0 if path has no directory part or does not fit. */
static int dir_of(const char *path, char *out, size_t n) {
  if (!path || !*path) return 0;
  const char *slash = strrchr(path, '/');
  if (!slash) return 0;
  const size_t dlen = (size_t)(slash - path);
  const int has_dev = strchr(path, ':') && strchr(path, ':') < slash;
  const int w = snprintf(out, n, "%s%.*s", has_dev ? "" : "sdmc:", (int)dlen, path);
  if (w < 0 || (size_t)w >= n) return 0;
  size_t l = strlen(out);
  while (l > 1 && out[l - 1] == '/') out[--l] = 0;
  return 1;
}

static int has_game(const char *dir) {
  char p[300]; struct stat st;
  snprintf(p, sizeof p, "%s/libunity.so", dir);
  return stat(p, &st) == 0;
}

void sd_home_init(int argc, char **argv) {
  char nro_dir[256] = "", cwd_dir[256] = "", cwd[256];

  if (argc > 0 && argv && argv[0] && dir_of(argv[0], nro_dir, sizeof nro_dir) && has_game(nro_dir)) {
    snprintf(s_home, sizeof s_home, "%s", nro_dir);
    snprintf(s_how, sizeof s_how, "the .nro's folder");
    return;
  }
  if (getcwd(cwd, sizeof cwd)) {
    size_t l = strlen(cwd);
    while (l > 1 && cwd[l - 1] == '/') cwd[--l] = 0;
    const int w = snprintf(cwd_dir, sizeof cwd_dir, "%s%s", strchr(cwd, ':') ? "" : "sdmc:", cwd);
    if (w > 0 && (size_t)w < sizeof cwd_dir && has_game(cwd_dir)) {
      snprintf(s_home, sizeof s_home, "%s", cwd_dir);
      snprintf(s_how, sizeof s_how, "the current directory");
      return;
    }
  }
  if (has_game(s_home)) return;                        /* the built-in default */
  if (nro_dir[0]) {                                    /* nothing found: report the .nro's folder */
    snprintf(s_home, sizeof s_home, "%s", nro_dir);
    snprintf(s_how, sizeof s_how, "the .nro's folder (no libunity.so)");
  }
}
