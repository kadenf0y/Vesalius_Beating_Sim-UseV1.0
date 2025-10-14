#pragma once
#include <Arduino.h>

/*
===============================================================================
  IO: Thin hardware accessors (ADC config, PWM, valve pin)
===============================================================================
*/

void     io_init();

uint16_t io_adc_read_vent();      // raw 12-bit counts
uint16_t io_adc_read_atr();

void     io_valve_write(bool forward); // true=forward, false=reverse
void     io_pwm_write(uint8_t pwm);    // 0..255
