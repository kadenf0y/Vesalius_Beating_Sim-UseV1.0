#include <Arduino.h>
#include "app_config.h"
#include "shared.h"
#include "io.h"
#include "buttons.h"
#include "control.h"
#include "web.h"
#include "flow.h"

/*
===============================================================================
  Boot — bring-up order:
   1) Serial
   2) Shared & IO
   3) Buttons
   4) Flow (ISR + task)
   5) Control task (Core 1)
   6) Web (Core 0): AP + HTTP + SSE + calibration
===============================================================================
*/

void setup() {
  Serial.begin(921600);
  delay(150);
  Serial.println("\n[BOOT] VSU-ESP32 starting…");

  shared_init();
  io_init();
  buttons_init();

  flow_begin();            // start ISR + sampler

  control_start_task();    // Core 1
  web_start();             // Core 0
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(200));
}
