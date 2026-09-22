/* sd_tilt.c -- Input.acceleration from the Switch's motion sensor or the left stick.
 *
 * WHAT THE GAME READS (disassembled, not assumed)
 *   SimpleGestureMonitor.Update/HandleTilt:  m_tilt = Input.acceleration   (raw, every frame)
 *   EnerbeamController.UpdateCharacterPosition: swing = clamp(m_tilt.x * sensitivity, -1, 1) * range
 *   Character.EnableBeamController / DisableBeamController -> Input.gyro.enabled = true / false
 * So the Enerbeam steers with the accelerometer's X -- the gravity component
 * across the screen, in g -- and the game switches the gyro on exactly while a
 * beam is active. Nothing fed the sensor before this file: the Enerbeam
 * tutorial passed with the value the engine happened to hold.
 *
 * UNITY'S CONVENTION: Input.acceleration is the gravity direction in the
 * device frame, in g: a phone held upright reads (0,-1,0), and rotating it
 * clockwise (right side down) by t gives x = +sin t. That is what is returned:
 * (x, -sqrt(1-x^2), 0).
 *
 * SOURCES (config.txt "tilt"):
 *   gyro  the controller's accelerometer. The lateral axis per controller:
 *         handheld and Joy-Con pair (Joy-Con IMU frame): +accel.x when tilted
 *         right -- sonicjump_nx's hardware-tested handheld mapping; the Pro
 *         Controller's IMU is mounted mirrored (nx_pointer.c measured the same
 *         for the gyro), so -accel.x. In the portrait modes the CONSOLE is turned,
 *         so handheld uses its y axis -- sonicjump_nx's mapping, verified on
 *         real hardware; its rotation 1/2 are the same physical holds as ours
 *         (the two ports' touch mappings are identical). The stick steering
 *         turns the same way (sd_tilt_stick_lateral).
 *         Straight ahead is the pose held over the ~1 s before a beam starts.
 *         Supported: handheld, Pro Controller, a Joy-Con pair (grip or held
 *         together). A single detached Joy-Con has no reading here: the stick steers.
 *   stick the left stick, only while a beam is active (it moves the cursor
 *         otherwise), full deflection = full lean.
 *   both  the stick while it is pushed, the motion sensor otherwise (default).
 * tilt_invert and tilt_sensitivity (percent) apply to either.
 * ------------------------------------------------------------------------- */
#include <math.h>
#include <string.h>

#include "config.h"
#include "sd_config.h"
#include "sd_tilt.h"
#include "util.h"

int   nxp_read_accel(float out[3], int *kind);   /* nx_pointer.c */
void  nxp_stick_xy(float *x, float *y);          /* nx_pointer.c: left stick, -1..1, y up */
int   nxp_is_handheld(void);                     /* nx_pointer.c: attached Joy-Cons this frame */

static volatile int s_beam;
static int   s_calibrate;          /* set when a beam starts: the pose held before it is straight ahead */
static float s_pose_avg;           /* the lateral pose, averaged over ~1 s while no beam is active */
static int   s_pose_samples;
static float s_neutral;
static float s_x;
static unsigned s_frames, s_beam_frames, s_reads_ok, s_reads_failed;
static float s_min, s_max;

/* The stick's "picture right" component. In the portrait modes the attached
 * Joy-Cons turn with the console, so their stick does too: held for rotation 1
 * (right Joy-Con up) the stick's own down (-y) points at the picture's right;
 * for rotation 2, its up (+y). This is the rotation the cursor already uses
 * (sd_tate_map_stick / sonicjump_nx's nxp_rot_delta, verified on hardware).
 * Docked controllers and detached Joy-Cons are held upright: straight through
 * (sonicjump_nx's handheld-only rule). Rotation 3 is held landscape. */
float sd_tilt_stick_lateral(float sx, float sy, int handheld, int rotation) {
  if (handheld && rotation == 1) return -sy;
  if (handheld && rotation == 2) return  sy;
  return sx;
}

float sd_tilt_lateral(const float a[3], int kind, int rotation) {
  if (kind == 1) return -a[0];                   /* Pro Controller: mirrored IMU frame */
  if (kind == 2) {                               /* handheld: the console itself is turned */
    if (rotation == 1) return a[1];              /* picture 90 CW: console held 90 CCW */
    if (rotation == 2) return -a[1];
    return a[0];
  }
  return a[0];                                   /* Joy-Con pair (grip): Joy-Con frame */
}

void sd_tilt_set_beam(int on) {
  on = on != 0;
  if (on && !s_beam) {
    s_calibrate = 1; s_beam_frames = 0; s_min = 1; s_max = -1;
    debugPrintf("[tilt] the game enabled the gyro (Enerbeam): the pose held now is straight ahead\n");
  } else if (!on && s_beam) {
    debugPrintf("[tilt] Enerbeam over after %u frames; x ranged %.2f .. %.2f\n", s_beam_frames,
                (double)(s_beam_frames ? s_min : 0), (double)(s_beam_frames ? s_max : 0));
  }
  s_beam = on;
}
int sd_tilt_beam(void) { return s_beam; }

static float clamp1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

void sd_tilt_update(void) {
  s_frames++;
  const int mode = sd_cfg_tilt;
  if (mode == SD_TILT_OFF) { s_x = 0.0f; return; }

  float stick = 0.0f; int stick_on = 0;
  if ((mode & SD_TILT_STICK) && s_beam) {
    float sx, sy; nxp_stick_xy(&sx, &sy);
    const float v = sd_tilt_stick_lateral(sx, sy, nxp_is_handheld(), sd_cfg_rotation), dz = 0.15f;
    if (fabsf(v) > dz) { stick = (fabsf(v) - dz) / (1.0f - dz) * (v < 0 ? -1.0f : 1.0f); stick_on = 1; }
  }
  float gyro = 0.0f; int gyro_on = 0; float a[3] = { 0 }; int kind = 0;
  const int got = (mode & SD_TILT_GYRO) ? nxp_read_accel(a, &kind) : 0;
  if (mode & SD_TILT_GYRO) { if (got) s_reads_ok++; else if (!s_reads_failed++) debugPrintf("[tilt] motion sensor read failed (no sample) -- see the periodic [tilt] line\n"); }
  if (got) {
    const float lat = sd_tilt_lateral(a, kind, sd_cfg_rotation);
    /* Straight ahead = how the console was held over the last second or so
     * BEFORE the beam: one sample at the beam's first frame catches the hand
     * mid-movement (a beam starts during play) and biases the whole beam. */
    if (!s_beam && !s_calibrate) {
      s_pose_avg = s_pose_samples ? s_pose_avg + (lat - s_pose_avg) * 0.05f : lat;
      if (s_pose_samples < 1000000) s_pose_samples++;
    }
    if (s_calibrate) {
      s_neutral = s_pose_samples >= 20 ? s_pose_avg : lat; s_calibrate = 0;
      debugPrintf("[tilt] neutral %.3f (%s; controller %s, accel %.2f %.2f %.2f)\n", (double)s_neutral,
                  s_pose_samples >= 20 ? "the pose held before the beam" : "first sample",
                  kind == 1 ? "Pro" : kind == 2 ? "handheld" : "Joy-Con pair", (double)a[0], (double)a[1], (double)a[2]);
    }
    gyro = lat - s_neutral; gyro_on = 1;
  }
  float x = stick_on ? stick : gyro_on ? gyro : 0.0f;
  x *= (float)sd_cfg_tilt_sens / 100.0f;
  if (sd_cfg_tilt_invert) x = -x;
  s_x = clamp1(x);

  /* Every ~5 s, beam or not: whether the sensor delivers and what the game is told. */
  if ((s_frames % 300) == 150)
    debugPrintf("[tilt] sensor %s, rotation %d%s: %u reads ok, %u failed; accel %.2f %.2f %.2f -> lateral %+.2f, game sees x=%+.2f (neutral %+.2f, beam %s)\n",
                kind == 1 ? "Pro Controller" : kind == 2 ? "handheld" : kind == 3 ? "Joy-Con pair" : "(none)",
                sd_cfg_rotation, (sd_cfg_rotation == 1 || sd_cfg_rotation == 2) ? (kind == 2 ? " (turned with the picture)" : " (controller upright: not turned)") : "",
                s_reads_ok, s_reads_failed, (double)a[0], (double)a[1], (double)a[2],
                (double)(got ? sd_tilt_lateral(a, kind, sd_cfg_rotation) : 0.0f), (double)s_x, (double)s_neutral, s_beam ? "on" : "off");
  if (s_beam) {
    s_beam_frames++;
    if (s_x < s_min) s_min = s_x;
    if (s_x > s_max) s_max = s_x;
    if ((s_beam_frames % 30) == 1)
      debugPrintf("[tilt] beam: x=%+.2f from %s (stick %+.2f, sensor %+.2f, accel %.2f %.2f %.2f)\n",
                  (double)s_x, stick_on ? "stick" : gyro_on ? "sensor" : "nothing", (double)stick,
                  (double)gyro, (double)a[0], (double)a[1], (double)a[2]);
  }
}

void sd_tilt_vector(float out[3]) {
  const float x = s_x;
  out[0] = x;
  out[1] = -sqrtf(1.0f - x * x > 0.0f ? 1.0f - x * x : 0.0f);
  out[2] = 0.0f;
}
