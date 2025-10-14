#pragma once
#include <Arduino.h>
#include <unity.h>
#include "app_config.h"

static void test_cal_helpers(){
  // identity gain with negative offset should pass simple sanity
  const float raw = 800.0f;
  const float a = scale_atrium(raw);
  const float v = scale_vent(raw);
  TEST_ASSERT_TRUE(isfinite(a));
  TEST_ASSERT_TRUE(isfinite(v));
}
