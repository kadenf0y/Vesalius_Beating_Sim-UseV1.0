#include <esp32-hal-ledc.h>
#include <esp32-hal-adc.h>
#include "io.h"

void io_begin(){
  // Valve
  pinMode(PIN_VALVE, OUTPUT);
  digitalWrite(PIN_VALVE, VALVE_FWD);

  // Pump PWM on LEDC
  ledcSetup(PUMP_LEDC_CH, PUMP_PWM_FREQ_HZ, PUMP_PWM_BITS);
  ledcAttachPin(PIN_PUMP_PWM, PUMP_LEDC_CH);
  ledcWrite(PUMP_LEDC_CH, 0);

  // ADC
  analogReadResolution(ADC_BITS);
  pinMode(PIN_PRESS_ATR, INPUT);
  pinMode(PIN_PRESS_VENT, INPUT);
  analogSetPinAttenuation(PIN_PRESS_ATR,  PRESS_ATTEN);
  analogSetPinAttenuation(PIN_PRESS_VENT, PRESS_ATTEN);

  // Flow input
  pinMode(PIN_FLOW, FLOW_INPUT_PULLUP ? INPUT_PULLUP : INPUT);
}

uint16_t io_read_atr(){  return analogRead(PIN_PRESS_ATR);  }
uint16_t io_read_vent(){ return analogRead(PIN_PRESS_VENT); }

void io_write_valve(uint8_t dir01){ digitalWrite(PIN_VALVE, dir01 ? HIGH : LOW); }
void io_write_pwm(uint8_t duty){    ledcWrite(PUMP_LEDC_CH, duty); }
