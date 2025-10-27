#include <Preferences.h>
#include "shared.h"

static QueueHandle_t g_q = nullptr;
Shared G;

// NVS keys
static const char* NS_CAL = "cal";

QueueHandle_t shared_cmdq(){ return g_q; }

bool shared_post(const Cmd& c){
  if (!g_q) return false;
  return xQueueSend(g_q, &c, 0) == pdTRUE;
}

static void load_cal_from_nvs(){
  Preferences p;
  // Open NVS namespace for read/write. Using read-write here avoids an
  // ESP-IDF-level NOT_FOUND log when the namespace hasn't been created yet
  // (first boot). We still treat failure as non-fatal and simply return.
  if (!p.begin(NS_CAL, false)) return;
  float am = p.getFloat("atr_m",  G.atr_m.load());
  float ab = p.getFloat("atr_b",  G.atr_b.load());
  float vm = p.getFloat("ven_m",  G.vent_m.load());
  float vb = p.getFloat("ven_b",  G.vent_b.load());
  float fm = p.getFloat("flo_m",  G.flow_m.load());
  float fb = p.getFloat("flo_b",  G.flow_b.load());
  p.end();
  G.atr_m.store(am);  G.atr_b.store(ab);
  G.vent_m.store(vm); G.vent_b.store(vb);
  G.flow_m.store(fm); G.flow_b.store(fb);
}

void shared_init(){
  if (!g_q) g_q = xQueueCreate(16, sizeof(Cmd));
  load_cal_from_nvs();
}
