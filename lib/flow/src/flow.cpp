#include "flow.h"
#include "shared.h"
#include "app_config.h"

static volatile uint32_t s_edges = 0;
static volatile uint32_t s_lastIsrUs = 0;

static void IRAM_ATTR flow_isr(){
  uint32_t now = micros();
  // simple 50 µs deglitch (20 kHz)
  if ((now - s_lastIsrUs) >= 50){
    s_edges++;
    s_lastIsrUs = now;
  }
}

static void flow_task(void*){
  uint32_t winMs = FLOW_MAX_WIN_MS;
  uint32_t t0 = millis();
  for(;;){
    uint32_t now = millis();
    if (now - t0 >= winMs){
      uint32_t dt = now - t0; t0 = now;
      noInterrupts();
      uint32_t e = s_edges; s_edges = 0;
      interrupts();

      float edges_per_s = (dt>0) ? (1000.0f * e / dt) : 0.0f;
      // both edges counted → Hz = edges/sec / 2
      float hz = edges_per_s * 0.5f;

      G.flow_hz.store(hz, std::memory_order_relaxed);

      // L/min = m*Hz + b (runtime cal)
      float lpm = G.flow_m.load()*hz + G.flow_b.load();
      if (lpm < 0) lpm = 0;
      G.flow_L_min.store(lpm, std::memory_order_relaxed);

      // new window targeting ~10 pulses (i.e., ~20 edges)
      if (edges_per_s > 0.1f){
        float target_s = FLOW_TARGET_EDGES / edges_per_s;
        uint32_t ms = (uint32_t)lroundf(target_s * 1000.0f);
        if (ms < FLOW_MIN_WIN_MS) ms = FLOW_MIN_WIN_MS;
        if (ms > FLOW_MAX_WIN_MS) ms = FLOW_MAX_WIN_MS;
        winMs = ms;
      }else{
        winMs = FLOW_MAX_WIN_MS;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void flow_begin(){
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), flow_isr,
                  FLOW_COUNT_BOTH_EDGES ? CHANGE : RISING);
  xTaskCreatePinnedToCore(flow_task, "flow", 2048, nullptr, 3, nullptr, CORE_CONTROL);
}
