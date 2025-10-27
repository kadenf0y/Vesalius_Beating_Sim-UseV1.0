#pragma once
#include <Arduino.h>
#include "app_config.h"

/* ==========================================================================================
   buttons.h — Debounced buttons on GPIO 14 (A) and 13 (B)
   Ownership:
     • Read on Core 1 (control loop).
     • Exposes debounced state and edges; implements short/long press logic in control.cpp.
   Wiring:
     • Buttons short to GND (active-LOW). Internal pull-ups enabled.
   ==========================================================================================*/

struct BtnState {
  bool aPressed=false, bPressed=false;
  bool aRise=false, aFall=false;
  bool bRise=false, bFall=false;
};

void buttons_init();
void buttons_read(BtnState& out); // call at 600 Hz
