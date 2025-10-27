#pragma once
#include <Arduino.h>

/* ==========================================================================================
   flow.h — Flow pulse counting
   Ownership:
     • ISR (any core) increments edge counter.
     • Core 1 background logic computes Hz on a dynamic window and updates G.flow_hz, G.flow_L_min.
   ==========================================================================================*/

void flow_begin();          // attach ISR, start background computation task on Core 1
