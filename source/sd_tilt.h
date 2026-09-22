/* sd_tilt.h -- Input.acceleration for the Enerbeam (sd_tilt.c). */
#ifndef SD_TILT_H
#define SD_TILT_H
enum { SD_TILT_OFF = 0, SD_TILT_GYRO = 1, SD_TILT_STICK = 2, SD_TILT_BOTH = 3 };
void sd_tilt_update(void);             /* once per frame, after the pad is polled       */
void sd_tilt_vector(float out[3]);     /* what Input.acceleration returns (in g)        */
void sd_tilt_set_beam(int on);         /* Input.gyro.enabled, set by the game           */
int  sd_tilt_beam(void);
/* The pure mapping, for tests: accelerometer sample (G) of controller `kind`
 * (1 Pro, 2 handheld, 3 Joy-Con pair) at `rotation` -> lateral tilt, +right. */
float sd_tilt_stick_lateral(float sx, float sy, int handheld, int rotation);
float sd_tilt_lateral(const float a[3], int kind, int rotation);
#endif
