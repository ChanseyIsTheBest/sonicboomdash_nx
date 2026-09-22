/* sd_pack.h -- glue between the asset pack (asset_pack.c), the I/O shim
 * (libc_shim.c) and boot (sd_pack_boot.c). */
#ifndef SD_PACK_H
#define SD_PACK_H

#include <stdio.h>

/* libc_shim.c ------------------------------------------------------------- */
/* Any spelling of a game path -> the pack key "assets/...", or NULL when the
 * path is not a packed game path. Accepts sdmc:/switch/<folder>/assets/...,
 * /switch/<folder>/assets/... and assets/... */
const char *nx_pack_relpath(const char *path);
/* 1 if a mounted pack holds this path as a file or a directory. */
int   sd_pack_exists(const char *path);
/* Read-only open served from the pack: a pack fd, or -1 (not packed). */
int   sd_pack_open_read(const char *path);
/* Size of a pack or real fd, -1 on error. */
long  sd_fd_size(int fd);
/* Positional read that is pack-aware (imports.c pread, __pread_chk). */
long  sd_pread(int fd, void *buf, size_t n, long off);
/* 1 if fd is an open pack handle (unity_imports.c z_dup, unity_jni.c). */
int   sd_is_pack_fd(int fd);
/* fdopen() that is pack-aware: a packed file becomes a memory stream. */
FILE *fdopen_fake(int fd, const char *mode);
int   closedir_fake(void *dirp);

/* nx_splitjoin.c ---------------------------------------------------------- */
void  nx_join_split_assets(const char *dir);

/* sd_pack_boot.c ---------------------------------------------------------- */
/* Mount the pack, or (first boot / new loose files) join the splits, build,
 * verify and mount it, then delete the loose tree (SD_PACK_DELETE_LOOSE).
 * Returns 1 when a pack is mounted. */
int   sd_pack_boot(void);

#endif /* SD_PACK_H */
