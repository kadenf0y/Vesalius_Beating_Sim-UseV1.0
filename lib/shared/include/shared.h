#pragma once
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "app_config.h"

/* ==========================================================================================
   shared.h — Cross-core shared state & commands
   ------------------------------------------------------------------------------------------
   Ownership:
     • Written on Core 1 (control): pwm/valve writes, telemetry updates, loopMs.
     • Read on Core 0 (web): SSE snapshot, HTTP handlers.
     • Commands posted from Core 0 → consumed on Core 1 via FreeRTOS queue.
   ==========================================================================================*/

struct Shared {
  // ---- Settings / state (UI-level) ----
  std::atomic<int>   mode{MODE_FWD};        // 0=FWD,1=REV,2=BEAT  (Core0 write via cmd → Core1 consumes)
  std::atomic<int>   paused{1};             // 0/1               (Core0 cmd / buttons on Core1)
  std::atomic<int>   pwmSet{0};             // 0..255 setpoint (Core0 cmd / buttons; Core1 ramps to this)
  std::atomic<int>   pwmOut{0};             // 0..255 actual hardware PWM (Core1 writes)
  std::atomic<int>   valve{VALVE_FWD};      // 0/1 current direction (Core1 writes; also raw override updates)
  std::atomic<int>   bpm{30};               // [1..60]           (Core0 cmd)

  // ---- Telemetry (calibrated where noted) ----
  std::atomic<float> atr_mmHg{0};           // calibrated (Core1 write)
  std::atomic<float> vent_mmHg{0};          // calibrated (Core1 write)
  std::atomic<float> flow_L_min{0};         // computed from flow Hz (Core1 write)
  std::atomic<float> loopMs{0};             // control loop EMA (Core1 write)

  // ---- Raw diagnostics ----
  std::atomic<int>   atr_raw{0};            // ADC counts (Core1 write)
  std::atomic<int>   vent_raw{0};           // ADC counts (Core1 write)
  std::atomic<float> flow_hz{0};            // edges/sec/2 (Core1 write)

  // ---- Calibration coefficients (runtime) ----
  std::atomic<float> atr_m{CAL_ATR_DEFAULT.m},  atr_b{CAL_ATR_DEFAULT.b};
  std::atomic<float> vent_m{CAL_VENT_DEFAULT.m},vent_b{CAL_VENT_DEFAULT.b};
  std::atomic<float> flow_m{CAL_FLOW_DEFAULT.m},flow_b{CAL_FLOW_DEFAULT.b};

  // ---- Calibration override gate (Core0 /cal writes; Core1 respects) ----
  std::atomic<int>      overrideOutputs{0};      // 1 = do not write to hardware from control loop
  std::atomic<uint32_t> overrideUntilMs{0};      // millis() deadline; refreshed by /cal raw ops
};
extern Shared G;

// ---- Commands (Core0 → Core1) ----
enum CmdType : uint8_t { CMD_TOGGLE, CMD_SET_PWM, CMD_SET_BPM, CMD_SET_MODE };
struct Cmd { CmdType t; int i; };

QueueHandle_t shared_cmdq();            // created in shared.cpp
void         shared_init();             // create queue, init NVS cal load
bool         shared_post(const Cmd&);   // non-blocking

// ---- Helpers applying calibration ----
static inline float apply_cal(float raw, float m, float b){ return m*raw + b; }
