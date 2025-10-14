#include "io.h"
#include "app_config.h"
#include <esp32-hal-adc.h>  // analogSetPinAttenuation

// LEDC channel/timer (keep stable across modules)
static const int LEDC_CH    = 0;
static const int LEDC_TIMER = 0;

void io_init() {
  // Valve default low
  pinMode(PIN_VALVE, OUTPUT);
  digitalWrite(PIN_VALVE, LOW);

  // PWM @ 20 kHz, 8-bit
  ledcSetup(LEDC_CH, 20000, 8);
  ledcAttachPin(PIN_PUMP_PWM, LEDC_CH);
  ledcWrite(LEDC_CH, 0);

  // ADC config — ADC1 pins so Wi-Fi coexistence is fine
  analogReadResolution(12);
  pinMode(PIN_PRESS_VENT, INPUT);
  pinMode(PIN_PRESS_ATR,  INPUT);
  analogSetPinAttenuation(PIN_PRESS_VENT, ADC_0db);
  analogSetPinAttenuation(PIN_PRESS_ATR,  ADC_0db);

  // Flow pin (external pull/level; no internal pullup)
  pinMode(PIN_FLOW, INPUT);
}

uint16_t io_adc_read_vent() { return analogRead(PIN_PRESS_VENT); }
uint16_t io_adc_read_atr()  { return analogRead(PIN_PRESS_ATR);  }
void     io_valve_write(bool fwd){ digitalWrite(PIN_VALVE, fwd ? HIGH : LOW); }
void     io_pwm_write(uint8_t pwm){ ledcWrite(LEDC_CH, pwm); }
