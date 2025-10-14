#include <Arduino.h>
#include "shared.h"
#include "app_config.h"
#include "flow.h"

/*
===============================================================================
  Flow sensor
  - ISR counts edges (RISING or FALLING per config), deglitches <100 µs.
  - Background task uses adaptive window to target ~30 pulses per window
    within [50..500] ms, computes Hz → mL/min, publishes G.flow_ml_min.
  - Optional PCNT backend can be wired under #if later.
===============================================================================
*/

#define FLOW_DEBUG 0 // set 1 for periodic prints

// ISR-shared counters
static volatile uint32_t s_pulses     = 0;
static volatile uint32_t s_lastMicros = 0;

// Last computed (debug/inspection)
static volatile uint32_t s_lastWindowMs = FLOW_WINDOW_MS_MAX;
static volatile uint32_t s_lastCnt      = 0;
static volatile float    s_lastHz       = 0.0f;

static void IRAM_ATTR flow_isr() {
  const uint32_t now = micros();
  if ((now - s_lastMicros) >= 100) { // ~10 kHz max edges
    s_pulses++;
    s_lastMicros = now;
  }
}

static inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static void flow_task(void*) {
  uint32_t window_ms = FLOW_WINDOW_MS_MAX;
  uint32_t t0 = millis();

#if FLOW_DEBUG
  uint32_t dbgT = 0;
#endif

  for (;;) {
    const uint32_t now = millis();

#if FLOW_DEBUG
    if ((now - dbgT) >= 200) {
      dbgT = now;
      const int lvl = digitalRead(PIN_FLOW);
      noInterrupts(); const uint32_t pending = s_pulses; interrupts();
      Serial.printf("[flow] pin=%d lvl=%d pending=%lu window=%lums\n",
                    PIN_FLOW, lvl, (unsigned long)pending, (unsigned long)window_ms);
    }
#endif

    if ((now - t0) >= window_ms) {
      const uint32_t elapsed = now - t0; t0 = now;

      noInterrupts(); uint32_t cnt = s_pulses; s_pulses = 0; interrupts();

      const float elapsed_s = elapsed / 1000.0f;
      const float hz        = (elapsed_s > 0.0f) ? (cnt / elapsed_s) : 0.0f;
      const float q_ml_min  = flow_hz_to_mlmin(hz);

      G.flow_ml_min.store(q_ml_min, std::memory_order_relaxed);

      s_lastWindowMs = elapsed; s_lastCnt = cnt; s_lastHz = hz;

#if FLOW_DEBUG
      Serial.printf("[flow] window=%lums cnt=%lu hz=%.3f q=%.1f\n",
                    (unsigned long)elapsed, (unsigned long)cnt, hz, q_ml_min);
#endif

      if (hz > 0.0f) {
        const uint32_t next_ms = (uint32_t) lroundf((FLOW_TARGET_PULSES / hz) * 1000.0f);
        window_ms = clampU32(next_ms, FLOW_WINDOW_MS_MIN, FLOW_WINDOW_MS_MAX);
      } else {
        window_ms = FLOW_WINDOW_MS_MAX;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void flow_begin() {
  // For now, assume external biasing; no internal pull
  pinMode(PIN_FLOW, INPUT);

  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flow_isr,
                  FLOW_EDGE_FALLING ? FALLING : RISING);

#if FLOW_DEBUG
  Serial.printf("[flow] begin (pin=%d, edge=%s)\n",
                PIN_FLOW, FLOW_EDGE_FALLING ? "FALLING" : "RISING");
#endif

  xTaskCreatePinnedToCore(flow_task, "flow", 2048, nullptr, 3, nullptr, CONTROL_CORE);
}
