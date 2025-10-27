#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/* ==========================================================================================
   web_cal.h — Calibration UI endpoints (mounted by web.cpp)
   Notes:
     • This module is UI-level only. It *calls back* into lambdas provided by web.cpp
       so we don’t depend on control internals here.
   ==========================================================================================*/

struct CalHooks {
  // Raw one-shot reads (immediate snapshots)
  int   (*read_atr_raw)();       // returns ADC counts
  int   (*read_vent_raw)();      // returns ADC counts
  float (*read_flow_hz)();       // returns Hz estimate

  // Raw output control (immediate)
  void  (*write_pwm_raw)(uint8_t duty);    // ALSO sets override gate (now+3s)
  void  (*write_valve_raw)(uint8_t dir01); // 0/1 (sets override gate)

  // Access to current calibration coefficients (get/set runtime; save/load)
  void  (*get_cals)(float& am,float& ab,float& vm,float& vb,float& fm,float& fb);
  void  (*set_cals)(float  am,float  ab,float  vm,float  vb,float  fm,float  fb);
  bool  (*nvs_save)(bool atr,bool vent,bool flow);
  bool  (*nvs_load)();
  void  (*nvs_defaults)();
};

void web_cal_register(AsyncWebServer& srv, const CalHooks& hooks);
