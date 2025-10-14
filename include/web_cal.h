#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/*
===============================================================================
  Web Calibration routes and tiny in-memory dataset

  Hooks remain for reading raw averages; manual override for PWM/valve is now
  implemented directly by /api/pwm and /api/valve in web_cal.cpp, which:
    - Apply raw hardware writes immediately (PWM 0..255, valve 0/1)
    - Set G.overrideOutputs=1 and refresh G.overrideUntilMs=now+3000ms so the
      control loop does not fight manual settings during calibration.
===============================================================================
*/

struct WebCalHooks {
  float (*readAtrRawOnce)();
  float (*readVentRawOnce)();
  int   (*getValveDir)();              // -1, 0, +1 (unused here; kept for compatibility)
  void  (*setValveDir)(int dir);       // negative=REV/0, positive=FWD/1 (unused; kept)
  void  (*setPwmRaw)(uint8_t pwm);     // 0..255 (unused; kept)
};

void web_cal_set_hooks(const WebCalHooks& hooks);
void web_cal_register_routes(AsyncWebServer& server);
