#include <Arduino.h>
#include "app_config.h"
#include "buttons.h"
#include "driver/gpio.h"

static const uint32_t DEBOUNCE_MS = 30;

struct Deb {
  bool raw = false;       // instantaneous read (active-LOW)
  bool stable = false;    // debounced state
  uint32_t lastFlip = 0;  // time raw last changed
  bool inited = false;
};

static Deb dA{}, dB{};

// active-LOW wiring
static inline bool readRawPressed(int pin) { return digitalRead(pin) == LOW; }

void buttons_init() {
  pinMode(PIN_BTN_A, INPUT_PULLUP);
  pinMode(PIN_BTN_B, INPUT_PULLUP);

  // Enforce pull-ups via LL driver also (robustness)
  gpio_pullup_en((gpio_num_t)PIN_BTN_A); gpio_pulldown_dis((gpio_num_t)PIN_BTN_A);
  gpio_pullup_en((gpio_num_t)PIN_BTN_B); gpio_pulldown_dis((gpio_num_t)PIN_BTN_B);

  const uint32_t now = millis();
  dA.raw = dA.stable = readRawPressed(PIN_BTN_A); dA.lastFlip = now; dA.inited = true;
  dB.raw = dB.stable = readRawPressed(PIN_BTN_B); dB.lastFlip = now; dB.inited = true;
}

static inline void process(Deb &d, bool newRaw, uint32_t now,
                           bool &pressed, bool &pressEdge, bool &releaseEdge) {
  pressEdge = releaseEdge = false;

  if (!d.inited) { d.raw = d.stable = newRaw; d.lastFlip = now; d.inited = true; }
  if (newRaw != d.raw) { d.raw = newRaw; d.lastFlip = now; }

  if (d.stable != d.raw && (now - d.lastFlip) >= DEBOUNCE_MS) {
    const bool old = d.stable;
    d.stable = d.raw;
    if ( d.stable && !old) pressEdge   = true;
    if (!d.stable &&  old) releaseEdge = true;
  }
  pressed = d.stable;
}

void buttons_read_debounced(BtnState &out) {
  const uint32_t now = millis();
  const bool rawA = readRawPressed(PIN_BTN_A);
  const bool rawB = readRawPressed(PIN_BTN_B);
  process(dA, rawA, now, out.aPressed, out.aPressEdge, out.aReleaseEdge);
  process(dB, rawB, now, out.bPressed, out.bPressEdge, out.bReleaseEdge);
}

void buttons_debug_print() {
  int rawA = digitalRead(PIN_BTN_A);
  int rawB = digitalRead(PIN_BTN_B);
  Serial.printf("[BTN] pins: A=%d raw=%d  B=%d raw=%d\n", PIN_BTN_A, rawA, PIN_BTN_B, rawB);
}
