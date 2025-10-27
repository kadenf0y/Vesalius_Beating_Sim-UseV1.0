#pragma once
#include <Arduino.h>
#include "app_config.h"

/* ==========================================================================================
   io.h — Thin, well-commented hardware IO wrappers
   Ownership:
     • All functions are USED from Core 1 (control).
     • Web (/cal raw) writes may bypass via override gate but still call these.
   ==========================================================================================*/

void io_begin();

// ADC raw (12-bit). Fast, non-blocking.
uint16_t io_read_atr();     // atrium raw counts
uint16_t io_read_vent();    // ventricle raw counts

// Actuators (respect raw values)
void io_write_valve(uint8_t dir01); // 0=FWD (LOW), 1=REV (HIGH)
void io_write_pwm(uint8_t duty);    // 0..255 (LEDC @ 6 kHz)
