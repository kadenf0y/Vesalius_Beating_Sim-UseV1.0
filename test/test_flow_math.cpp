#include <Arduino.h>
#include <unity.h>
#include "app_config.h"

// Header-only helpers (no device IO). Lightweight checks for math/limits.

static float edges_to_hz(uint32_t edges, float window_s){
  float edges_per_s = (window_s>0)? (edges / window_s) : 0.0f;
  return edges_per_s * (FLOW_COUNT_BOTH_EDGES ? 0.5f : 1.0f);
}

void test_edges_to_hz(){
  // 20 edges in 0.5 s = 40 edges/s → 20 Hz (both edges → /2)
  TEST_ASSERT_FLOAT_WITHIN(0.01, 20.0f, edges_to_hz(20, 0.5f));
}
void test_hz_to_lpm(){
  // Hz/23.6
  TEST_ASSERT_FLOAT_WITHIN(0.001, 1.0f, hz_to_lpm(23.6f));
}
void test_window_clamps(){
  TEST_ASSERT_EQUAL_UINT32(FLOW_MIN_WIN_MS, (uint32_t) (1000/60));
  TEST_ASSERT_EQUAL_UINT32(FLOW_MAX_WIN_MS, (uint32_t) (1000/6));
}

void setup(){
  UNITY_BEGIN();
  RUN_TEST(test_edges_to_hz);
  RUN_TEST(test_hz_to_lpm);
  RUN_TEST(test_window_clamps);
  UNITY_END();
}
void loop(){}
