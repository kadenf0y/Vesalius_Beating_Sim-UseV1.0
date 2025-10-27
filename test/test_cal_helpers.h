#pragma once
// Minimal header-only checks (can be included in other tests if expanded)
#include <math.h>
static inline void linfit_2pt(float x1,float y1,float x2,float y2,float& m,float& b){
  if (x2==x1){ m=1; b=0; return; }
  m = (y2-y1)/(x2-x1); b = y1 - m*x1;
}
