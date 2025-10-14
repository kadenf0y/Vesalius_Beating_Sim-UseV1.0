#pragma once
#include <Arduino.h>
#include "app_config.h"

/*
===============================================================================
  Buttons: two active-LOW momentary inputs with debounce + edge flags
===============================================================================
*/

void buttons_init();

struct BtnState {
  bool aPressed{};
  bool bPressed{};
  bool aPressEdge{};
  bool aReleaseEdge{};
  bool bPressEdge{};
  bool bReleaseEdge{};
};

void buttons_read_debounced(BtnState& out);
void buttons_debug_print();
