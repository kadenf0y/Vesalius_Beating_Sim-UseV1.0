#include <Arduino.h>
#include "app_config.h"
#include "shared.h"
#include "io.h"
#include "buttons.h"
#include "flow.h"
#include "control.h"
#include "web.h"

void setup(){
  Serial.begin(921600);
  delay(100);
  Serial.println("\n[BOOT] SimUse ESP32");

  shared_init();
  io_begin();
  buttons_init();
  flow_begin();            // ISR + windowing task (Core 1)

  // Start tasks
  control_start();         // 600 Hz on Core 1
  web_start();             // Web/SSE on Core 0
}

void loop(){
  // Idle — all work in tasks
  vTaskDelay(pdMS_TO_TICKS(250));
}
