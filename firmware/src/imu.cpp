#include "imu.h"
#include "display_cfg.h"
#include <Arduino.h>

// Poll and hysteresis timing
#define IMU_POLL_MS       100    // read accel at ~10 Hz
#define STABLE_TIME_MS    300    // orientation must be stable this long before rotating
#define TILT_THRESHOLD    0.5f   // ~30 degrees from axis (sin(30) ~ 0.5)

static uint8_t  current_rotation = 0;
static uint8_t  candidate_rotation = 0;
static uint32_t candidate_since = 0;
static uint32_t last_poll_ms = 0;
static bool     imu_ok = false;

// The QMI8658 sits at different physical orientations on different boards.
// CLAWD_IMU_AXES picks the transform from raw chip axes → screen axes.
// If rotation is wrong on hardware, try the next number in this list and
// rebuild — one of them will line up. Default for each board is a guess.
//
//   0: screen_x =  ax, screen_y =  ay   (S3 default — was working)
//   1: screen_x =  ay, screen_y =  ax
//   2: screen_x = -ay, screen_y =  ax
//   3: screen_x =  ay, screen_y = -ax
//   4: screen_x = -ax, screen_y = -ay   (S3 rotated 180°)
//   5: screen_x = -ax, screen_y =  ay
//   6: screen_x =  ax, screen_y = -ay
//   7: screen_x = -ay, screen_y = -ax
#ifndef CLAWD_IMU_AXES
#  if defined(CLAWD_BOARD_C6)
#    define CLAWD_IMU_AXES 1
#  else
#    define CLAWD_IMU_AXES 0
#  endif
#endif

static inline void imu_axes_transform(float &ax, float &ay) {
    float a = ax, b = ay;
#if   CLAWD_IMU_AXES == 0
    ax =  a;  ay =  b;
#elif CLAWD_IMU_AXES == 1
    ax =  b;  ay =  a;
#elif CLAWD_IMU_AXES == 2
    ax = -b;  ay =  a;
#elif CLAWD_IMU_AXES == 3
    ax =  b;  ay = -a;
#elif CLAWD_IMU_AXES == 4
    ax = -a;  ay = -b;
#elif CLAWD_IMU_AXES == 5
    ax = -a;  ay =  b;
#elif CLAWD_IMU_AXES == 6
    ax =  a;  ay = -b;
#elif CLAWD_IMU_AXES == 7
    ax = -b;  ay = -a;
#else
#  error "Invalid CLAWD_IMU_AXES value (0..7)"
#endif
}

// Determine target rotation from accelerometer gravity vector.
// Returns 0-3 or 255 if ambiguous (e.g. face-up/face-down).
static uint8_t accel_to_rotation(float ax, float ay) {
    imu_axes_transform(ax, ay);

    float abs_ax = fabsf(ax);
    float abs_ay = fabsf(ay);

    if (abs_ax < TILT_THRESHOLD && abs_ay < TILT_THRESHOLD) {
        return 255;  // ambiguous, keep current
    }

    if (abs_ay > abs_ax) {
        return (ay > 0) ? 3 : 1;
    } else {
        return (ax > 0) ? 0 : 2;
    }
}

void imu_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK");

    imu.configAccelerometer(
        SensorQMI8658::ACC_RANGE_4G,
        SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();

    imu_ok = true;
}

void imu_tick(void) {
    if (!imu_ok) return;

    uint32_t now = millis();
    if (now - last_poll_ms < IMU_POLL_MS) return;
    last_poll_ms = now;

    float ax, ay, az;
    if (!imu.getAccelerometer(ax, ay, az)) return;

    uint8_t target = accel_to_rotation(ax, ay);
    if (target == 255 || target == current_rotation) {
        candidate_rotation = current_rotation;
        return;
    }

    if (target != candidate_rotation) {
        candidate_rotation = target;
        candidate_since = now;
    } else if (now - candidate_since >= STABLE_TIME_MS) {
        current_rotation = target;
        Serial.printf("Rotation: %d\n", current_rotation);
    }
}

uint8_t imu_get_rotation(void) {
    return current_rotation;
}
