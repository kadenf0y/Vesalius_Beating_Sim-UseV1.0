#pragma once
#include <Arduino.h>

/* ==========================================================================================
   app_config.h  —  Central tunables, hardware map, and compile-time switches
   ------------------------------------------------------------------------------------------
   • This header is included by all modules. Keep it small and constexpr where possible.
   • C++14 only (no inline variables). Prefer static constexpr and static inline helpers.
   ==========================================================================================*/

// ===== Project identity =====
static constexpr const char* kApSsid = "SimUse-ESP32";     // SoftAP SSID (open)
static constexpr uint16_t    kHttpPort = 80;

// ===== Core assignment (ESP32): 0 = PRO (Wi-Fi), 1 = APP
static constexpr uint8_t CORE_WEB     = 0;   // Web/SSE/Networking
static constexpr uint8_t CORE_CONTROL = 1;   // Real-time control + IO

// ===== Control timing =====
static constexpr uint32_t CONTROL_HZ       = 600;            // main loop @ 600 Hz
static constexpr float    CONTROL_DT_S     = 1.0f / CONTROL_HZ;
static constexpr uint32_t SSE_HZ           = 60;             // stream at fixed 60 Hz
static constexpr uint32_t FLOW_MIN_WIN_MS  = 1000 / 60;      // 1/60 s lower clamp
static constexpr uint32_t FLOW_MAX_WIN_MS  = 1000 / 6;       // 1/6  s upper clamp
static constexpr float    FLOW_TARGET_EDGES= 10.0f * 2.0f;   // aim ~10 pulses → ~20 edges

// ===== LEDC PWM (pump) =====
// 6 kHz carrier, 8-bit resolution, duty 0..255 matches requested behavior.
static constexpr uint32_t PUMP_PWM_FREQ_HZ = 6000;
static constexpr uint8_t  PUMP_PWM_BITS    = 8;
static constexpr uint8_t  PUMP_LEDC_TIMER  = 0;
static constexpr uint8_t  PUMP_LEDC_CH     = 0;

// ===== Pins (ESP32 Dev Module) =====
static constexpr uint8_t PIN_PUMP_PWM      = 19;  // LEDC out
static constexpr uint8_t PIN_VALVE         = 23;  // digital out (0=FWD, 1=REV)
static constexpr uint8_t PIN_PRESS_ATR     = 32;  // ADC1
static constexpr uint8_t PIN_PRESS_VENT    = 33;  // ADC1
static constexpr uint8_t PIN_FLOW          = 27;  // pulse in
static constexpr uint8_t PIN_BTN_A         = 14;  // Play/Pause (active LOW)
static constexpr uint8_t PIN_BTN_B         = 13;  // PWM ± (active LOW)
static constexpr uint8_t PIN_STATUS_LED    = 2;   // status blink

// ===== Polarity & conventions =====
static constexpr uint8_t VALVE_FWD = 0;   // write LOW for Forward
static constexpr uint8_t VALVE_REV = 1;   // write HIGH for Reverse
static constexpr bool FLOW_COUNT_BOTH_EDGES = true; // count both edges
static constexpr bool FLOW_INPUT_PULLUP     = true; // enable internal PU if needed

// ===== ADC configuration =====
// Both pressure channels use ADC attenuation ADC_2_5db (~1.5 V FS)
static constexpr adc_attenuation_t PRESS_ATTEN = ADC_2_5db;
static constexpr uint8_t ADC_BITS = 12;

// ===== Smoothing =====
// 10-sample moving average at 600 Hz; each tick contributes 1 sample → updated per tick.
static constexpr uint8_t PRESS_SMOOTH_N = 10;

// ===== Mode numbers =====
enum : int { MODE_FWD=0, MODE_REV=1, MODE_BEAT=2 };

// ===== Ramps & beat sequencing =====
static constexpr uint32_t RAMP_MS      = 150;  // set→0 and 0→set ramps
static constexpr uint32_t DEAD_MS      = 100;  // dead time on flips
// Beat half-period T/2 = 60000/(2*BPM) ms. Sequence: Ramp(150) → Dead(100) → Ramp(150) → Hold(remaining).

// ===== PWM range =====
static constexpr uint8_t  PWM_MIN = 0;
static constexpr uint8_t  PWM_MAX = 255;

// ===== Flow conversion =====
// Edges/sec → Hz by dividing by 2 (counting both edges). Flow [L/min] = Hz / 23.6
static constexpr float FLOW_HZ_PER_LPM = 23.6f;

// ===== Calibration defaults (y = m*x + b) =====
struct Cal2 { float m, b; };
static constexpr Cal2 CAL_ATR_DEFAULT  { 1.0f, 0.0f };
static constexpr Cal2 CAL_VENT_DEFAULT { 1.0f, 0.0f };
static constexpr Cal2 CAL_FLOW_DEFAULT { 1.0f/23.6f, 0.0f }; // default: L/min = Hz/23.6

// ===== Web UI color & ranges (mirrored in UI object too) =====
static constexpr float UI_ATR_MIN   = -5.0f,  UI_ATR_MAX   = 205.0f;
static constexpr float UI_VENT_MIN  = -5.0f,  UI_VENT_MAX  = 205.0f;
static constexpr float UI_FLOW_MIN  = 0.0f,   UI_FLOW_MAX  = 7.5f;     // L/min
static constexpr float UI_PWM_MIN   = 0.0f,   UI_PWM_MAX   = 256.0f;
static constexpr float UI_VALVE_MIN = 0.0f,   UI_VALVE_MAX = 1.0f;

// ===== Build-time helpers =====
static inline float hz_to_lpm(float hz){ return hz / FLOW_HZ_PER_LPM; }
