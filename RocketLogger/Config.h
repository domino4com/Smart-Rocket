#pragma once
#include <stdint.h>
// CWA v2+ pin map. SD uses the expansion connector in native four-bit mode.
constexpr int SDA_PIN = 17, SCL_PIN = 18, PIXEL_PIN = 39, BUTTON_PIN = 0;
constexpr int SD_CLK = 36, SD_CMD = 37, SD_D0 = 38, SD_D1 = 34, SD_D2 = 33, SD_D3 = 35;
// Measured launch orientation is X positive; invert X to arm.
constexpr float ARM_AXIS_G = -0.75f;
// Physical inversion measured about -1.2g versus +0.9g upright on this sensor.
// Allow its offset/noise without treating strong acceleration as static gravity.
constexpr float ARM_GRAVITY_MIN_G = 0.65f, ARM_GRAVITY_MAX_G = 1.50f;
// ±16g maximizes headroom for motor thrust and landing shock. Values beyond
// this range clip; a higher-g sensor is needed if future motors exceed it.
constexpr uint16_t ACCEL_HZ = 800;
// PL053450 3.7V 900mAh LiPo: use conservative 100mA CC / 4.2V CV.
// Confirm the cell manufacturer charge rating before increasing this current.
constexpr bool ENABLE_CHARGING = true;
constexpr uint16_t CHARGE_CURRENT_MA = 100;

static_assert(CHARGE_CURRENT_MA == 100 || CHARGE_CURRENT_MA == 200,
              "Update the PMU current mapping before choosing a different charge current");
static_assert(ACCEL_HZ == 400 || ACCEL_HZ == 800 || ACCEL_HZ == 1600,
              "Supported tested ODR register mappings are 400, 800, and 1600 Hz");
