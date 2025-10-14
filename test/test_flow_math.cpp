#include <Arduino.h>
#include <unity.h>
#include "app_config.h"

static void test_flow_conversion(){
  // 23.6 Hz → 1000 mL/min by definition of constant
  const float mlmin = flow_hz_to_mlmin(23.6f);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, mlmin);

  // Zero Hz → zero flow
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, flow_hz_to_mlmin(0.0f));

  // 11.8 Hz → ~500 mL/min
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 500.0f, flow_hz_to_mlmin(11.8f));
}

void setup(){
  UNITY_BEGIN();
  RUN_TEST(test_flow_conversion);
  UNITY_END();
}
void loop(){}
