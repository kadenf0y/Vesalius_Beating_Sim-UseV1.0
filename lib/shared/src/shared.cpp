#include "shared.h"

static QueueHandle_t g_cmdq = nullptr;
Shared G;

void shared_init() {
  if (!g_cmdq) g_cmdq = xQueueCreate(8, sizeof(Cmd)); // small, fast
}

QueueHandle_t shared_cmdq() { return g_cmdq; }

bool shared_cmd_post(const Cmd& c) {
  if (!g_cmdq) return false;
  return xQueueSend(g_cmdq, &c, 0) == pdTRUE;
}
