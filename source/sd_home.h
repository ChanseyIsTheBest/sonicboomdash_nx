/* sd_home.h -- the game folder, found at run time (sd_home.c). */
#ifndef SD_HOME_H
#define SD_HOME_H

/* Resolve the folder from argv[0] (the .nro's own path), else the current
 * directory, else the built-in default. Call first thing in main(), before
 * anything logs: the log file lives in this folder. */
void        sd_home_init(int argc, char **argv);
const char *sd_home(void);        /* "sdmc:/switch/<any folder>" (no trailing '/') */
const char *sd_home_bare(void);   /* the same without the device: "/switch/<any folder>" */
const char *sd_home_how(void);    /* how it was found, for the boot log */
const char *sd_log_path(void);    /* <home>/debug.log */

#endif /* SD_HOME_H */
