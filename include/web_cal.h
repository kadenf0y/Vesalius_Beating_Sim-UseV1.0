#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/*
===============================================================================
  Web Calibration routes and tiny in-memory dataset
  - Hooks: app supplies raw readers and low-level actuators
  - Routes mounted by web.cpp
===============================================================================
*/

struct WebCalHooks {
  float (*readAtrRawOnce)();
  float (*readVentRawOnce)();
  int   (*getValveDir)();              // -1, 0, +1 (or 0/1 mapped)
  void  (*setValveDir)(int dir);       // negative=REV/0, positive=FWD/1
  void  (*setPwmRaw)(uint8_t pwm);     // 0..255
};

void web_cal_set_hooks(const WebCalHooks& hooks);
void web_cal_register_routes(AsyncWebServer& server);
