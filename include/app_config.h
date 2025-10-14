#pragma once
#include <Arduino.h>

/*
===============================================================================
  Vesalius SimUse (ESP32) — Application Configuration (C++14-safe)
  - All compile-time knobs and hardware mappings live here.
  - Keep helpers header-only & 'static inline' for zero-cost use across modules.
===============================================================================
*/

//--------------------------- Calibration defaults -----------------------------
// Linear scale: y_calibrated = gain * x_raw + offset
struct PressCal { float gain; float offset; };

static constexpr PressCal CAL_ATR  = { 1.000f, 00.0f };
static constexpr PressCal CAL_VENT = { 1.000f, 00.0f };

// Helpers (used when streaming; control loop uses raw unless documented otherwise)
static inline float scale_atrium(float raw) { return raw * CAL_ATR.gain  + CAL_ATR.offset; }
static inline float scale_vent  (float raw) { return raw * CAL_VENT.gain + CAL_VENT.offset; }

//----------------------------- Hardware mapping -------------------------------
/* Pins (ESP32 DevKit-ish). Buttons are momentary-to-GND (active-LOW). */
#define PIN_BTN_A        14     // Pause
#define PIN_BTN_B        13     // Power nudge
#define PIN_VALVE        23     // Valve direction (MOSFET driver)
#define PIN_PUMP_PWM     19     // LEDC PWM to pump driver
#define PIN_PRESS_VENT   32     // ADC1
#define PIN_PRESS_ATR    33     // ADC1
#define PIN_FLOW         27     // Flow sensor digital pulses
#define PIN_LED           2     // Onboard LED

//--------------------------- Control loop & PWM -------------------------------
#define CONTROL_HZ       500    // 500 Hz control loop (2 ms)
#define PWM_MAX          255    // 8-bit LEDC
#define PWM_FLOOR        165    // floor to avoid stall (empirical)
#define PWM_SLOPE_UP     350.0f // PWM counts per second up-ramp
#define PWM_SLOPE_DOWN   350.0f // PWM counts per second down-ramp
#define DEADTIME_MS      100    // valve settle time on direction flips

#ifndef STATUS_MS
#define STATUS_MS        200    // status print cadence (~5 Hz)
#endif

//------------------------------ Flow sensor -----------------------------------
// Edge polarity: true = FALLING (idle HIGH), false = RISING (idle LOW)
static constexpr bool FLOW_EDGE_FALLING = true;

// Adaptive sampler aims ~30 pulses/window within [50..500] ms
static constexpr uint16_t FLOW_WINDOW_MS_MIN = 50;
static constexpr uint16_t FLOW_WINDOW_MS_MAX = 500;
static constexpr float    FLOW_TARGET_PULSES = 30.0f;

// Typical DIGITEN/other hall wheel: ~23.6 Hz per 1 L/min => mL/min = Hz * (1000/23.6)
static constexpr float FLOW_HZ_PER_LPM   = 23.6f;
static constexpr float FLOW_HZ_TO_ML_MIN = 1000.0f / FLOW_HZ_PER_LPM;

static inline float flow_hz_to_mlmin(float hz) { return hz * FLOW_HZ_TO_ML_MIN; }

//------------------------------ Web & cores -----------------------------------
#define CORE_PRO         0
#define CORE_APP         1
#define CONTROL_CORE     CORE_APP
#define WEB_CORE         CORE_PRO

//------------------------------- Safety gate ----------------------------------
#ifndef ENABLE_OUTPUTS
#define ENABLE_OUTPUTS   1     // 0 = dry-run (no actuation)
#endif
