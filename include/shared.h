#pragma once
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "app_config.h"

/*
===============================================================================
  Shared state (atomics) + command queue for inter-task/core communication
  - Writers:
      CONTROL task: pwm, valve, vent_mmHg, atr_mmHg, flow_ml_min, loopMs
      WEB task (via queue): posts commands; may inline-apply if queue full
      FLOW task: flow_ml_min
  - Readers:
      WEB (SSE), CONTROL, tests
===============================================================================
*/

enum CmdType : uint8_t {
  CMD_TOGGLE_PAUSE = 1,
  CMD_SET_POWER_PCT,
  CMD_SET_MODE,
  CMD_SET_BPM
};

struct Cmd {
  CmdType type;
  int     iarg;     // power %, mode, etc.
  float   farg;     // bpm, etc.
};

struct Shared {
  // Commands / params (written by CONTROL and via queue)
  std::atomic<int>    paused{1};         // 1=paused, 0=running
  std::atomic<int>    powerPct{30};      // 10..100%
  std::atomic<int>    mode{0};           // 0=FWD, 1=REV, 2=BEAT
  std::atomic<float>  bpm{5.0f};         // beats per minute

  // Outputs (CONTROL task writes)
  std::atomic<unsigned> pwm{0};          // 0..255 raw LEDC
  std::atomic<unsigned> valve{0};        // 1=forward, 0=reverse (binary)

  // Telemetry (CONTROL + FLOW tasks write)
  std::atomic<float>  vent_mmHg{0.0f};   // NOTE: currently CONTROL writes *converted* mmHg
  std::atomic<float>  atr_mmHg{0.0f};
  std::atomic<float>  flow_ml_min{0.0f};

  // Performance (CONTROL writes)
  std::atomic<float>  loopMs{2.0f};
};

extern Shared G;

// Init queue & state; get queue; post command
void         shared_init();
QueueHandle_t shared_cmdq();
bool         shared_cmd_post(const Cmd& c);
