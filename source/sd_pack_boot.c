/* sd_pack_boot.c -- first-boot asset pack flow.
 *
 * The sequence is fruitninjaclassic_nx's (main.c, "round 82"), moved out of
 * main.c into its own file:
 *
 * All paths are under the game folder, found at run time (sd_home.c).
 *
 *   pack present, no loose tree  -> mount it (every normal boot)
 *   loose tree present           -> join the .splitN sets, build assets.nxpack,
 *                                   verify it by reading it back, mount it,
 *                                   then delete the loose tree (SD_PACK_DELETE_LOOSE)
 *   neither                      -> return 0; check_data() names what is missing
 *
 * Two safety points added for this port:
 *
 *   1. A loose tree beside an existing pack means files were copied in again --
 *      a game update, or a re-copy -- so the pack is REBUILT from them rather
 *      than silently serving the old data. (Only with SD_PACK_DELETE_LOOSE: with
 *      deletion off the loose tree is always there, so a valid pack is kept.)
 *   2. Deletion removes globalgamemanagers FIRST. It is the marker for "a
 *      complete loose tree", so a crash or power loss half-way through deleting
 *      leaves a tree that can never be mistaken for complete and packed short.
 *
 * Runs before the engine exists, on newlib directly (not through the shim).
 * The progress console (error.c) is released before returning, so the engine
 * can take the window for EGL.
 */
#include <switch.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "asset_pack.h"
#include "sd_pack.h"

/* <game folder>/<rel>. The folder is found at run time (sd_home.c), so paths
 * are built here rather than pasted together at compile time. */
#define PATH_MAX_SD 300
static const char *home_path(char *buf, const char *rel) {
  snprintf(buf, PATH_MAX_SD, "%s/%s", GAME_HOME, rel);
  return buf;
}
#define LOOSE_MARKER_REL "assets/bin/Data/globalgamemanagers"

static int exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

static void tree_stats(const char *path, unsigned *nfiles, uint64_t *nbytes) {
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *e; char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) tree_stats(child, nfiles, nbytes);
    else { (*nfiles)++; *nbytes += (uint64_t)st.st_size; }
  }
  closedir(d);
}

#if SD_ASSET_PACK && SD_PACK_DELETE_LOOSE
static void rmtree(const char *path) {
  DIR *d = opendir(path);
  if (!d) { unlink(path); return; }
  struct dirent *e;
  char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rmtree(child);
    else unlink(child);
  }
  closedir(d);
  rmdir(path);
}
#endif

/* Unity and IL2CPP WRITE beside the data at runtime. The pack is read-only, so
 * the empty directories are recreated at every boot for those writes to land in
 * (fruitninjaclassic_nx: without them Unity decided the data contained no Unity
 * data and raised its fatal dialog). Not il2cpp/...: Unity stages into il2cpp_tmp/
 * and renames into place, and a rename onto an existing directory fails. */
static void create_skeleton(void) {
  static const char *dirs[] = {
    "assets",
    "assets/bin",
    "assets/bin/Data",
    "assets/bin/Data/Managed",
    "assets/bin/Data/Managed/Metadata",
    "assets/bin/Data/Managed/Resources",
    "assets/bin/Data/Resources",
  };
  char b[PATH_MAX_SD];
  for (unsigned i = 0; i < sizeof dirs / sizeof *dirs; i++) mkdir(home_path(b, dirs[i]), 0777);   /* EEXIST is fine */
}

int sd_pack_boot(void) {
  char b[PATH_MAX_SD];
#if !SD_ASSET_PACK
  nx_join_split_assets(home_path(b, "assets/bin/Data"));
  return 0;
#else
  const int loose = exists(home_path(b, LOOSE_MARKER_REL));

#if SD_PACK_DELETE_LOOSE
  const int want_build = loose;                          /* safety point 1 */
#else
  int want_build = 0;                                    /* keep a valid pack; build only from a real tree */
  if (!asset_pack_open_existing(GAME_HOME)) want_build = loose;
#endif
  if (!want_build) {
    if (asset_pack_active() || asset_pack_open_existing(GAME_HOME)) {
#if SD_PACK_DELETE_LOOSE
      /* A crash or power loss after the marker went (safety point 2) leaves the
       * rest of the loose tree behind. level0 is game data Unity never writes,
       * so its presence means an interrupted deletion: finish it -- the mounted
       * pack is the complete copy. */
      if (exists(home_path(b, "assets/bin/Data/level0"))) {
        debugPrintf("[pack] finishing an interrupted delete of the loose assets/ tree\n");
        rmtree(home_path(b, "assets"));
      }
#endif
      create_skeleton();
      debugPrintf("[pack] mounted assets.nxpack: %zu entries\n", asset_pack_entry_count());
      return 1;
    }
    debugPrintf("[pack] no pack and no loose assets/: %s\n", asset_pack_error());
    return 0;                                            /* check_data() says what is missing */
  }

  if (exists(home_path(b, "assets.nxpack")))
    debugPrintf("[pack] loose assets/ found beside an existing pack -- rebuilding from the "
                "loose files (a game update or a re-copy)\n");
  startup_status_begin("Preparing game files");
  nx_join_split_assets(home_path(b, "assets/bin/Data"));

  unsigned nf = 0; uint64_t tot = 0;
  tree_stats(home_path(b, "assets"), &nf, &tot);
  debugPrintf("[pack] loose tree: %u files, %llu MB\n", nf, (unsigned long long)(tot >> 20));
  struct statvfs vfs;
  if (statvfs(GAME_HOME, &vfs) == 0) {
    const uint64_t freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    debugPrintf("[pack] SD free: %llu MB (the pack needs ~%llu MB)\n",
                (unsigned long long)(freeb >> 20), (unsigned long long)(tot >> 20));
    if (freeb < tot + (64u << 20)) {
      char msg[160];
      snprintf(msg, sizeof msg, "Not enough free space on the SD card:\n  need ~%llu MB, have %llu MB",
               (unsigned long long)((tot + (64u << 20)) >> 20), (unsigned long long)(freeb >> 20));
      startup_status_update(msg);
      debugPrintf("[pack] *** WARNING: not enough free space; the build will likely fail ***\n");
    }
  }

  debugPrintf("[pack] build: starting\n");
  if (!asset_pack_build(home_path(b, "assets"), GAME_HOME)) {   /* mounts it on success */
    debugPrintf("[pack] build FAILED: %s -- running from the loose files (slower)\n",
                asset_pack_error());
    startup_status_end();
    return 0;
  }
  debugPrintf("[pack] built, verified and mounted: %zu entries\n", asset_pack_entry_count());

#if SD_PACK_DELETE_LOOSE
  startup_status_update("Removing the unpacked asset files");
  unlink(home_path(b, LOOSE_MARKER_REL));                /* safety point 2: the marker first */
  rmtree(home_path(b, "assets"));
  debugPrintf("[pack] loose assets/ deleted (%u files)\n", nf);
#endif
  create_skeleton();
  startup_status_end();
  return 1;
#endif
}
