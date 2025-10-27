#include "buttons.h"

static constexpr uint32_t DEBOUNCE_MS = 25;

struct Deb {
  bool raw=false, st=false;
  uint32_t lastFlip=0;
  bool edgeRise=false, edgeFall=false;
};

static Deb da, db;

static inline bool readA(){ return digitalRead(PIN_BTN_A)==LOW; }
static inline bool readB(){ return digitalRead(PIN_BTN_B)==LOW; }

static void deb_process(Deb& d, bool nowL, uint32_t ms){
  d.edgeRise = d.edgeFall = false;
  if (nowL != d.raw){ d.raw = nowL; d.lastFlip = ms; }
  if ((ms - d.lastFlip) >= DEBOUNCE_MS && d.st != d.raw){
    d.edgeRise = (!d.st && d.raw);
    d.edgeFall = ( d.st && !d.raw);
    d.st = d.raw;
  }
}

void buttons_init(){
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);
  uint32_t ms=millis();
  da.raw=da.st=readA(); da.lastFlip=ms;
  db.raw=db.st=readB(); db.lastFlip=ms;
}

void buttons_read(BtnState& out){
  uint32_t ms=millis();
  deb_process(da, readA(), ms);
  deb_process(db, readB(), ms);
  out.aPressed = da.st; out.aRise = da.edgeRise; out.aFall = da.edgeFall;
  out.bPressed = db.st; out.bRise = db.edgeRise; out.bFall = db.edgeFall;
}
