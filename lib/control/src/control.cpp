#include <Arduino.h>
#include "app_config.h"
#include "control.h"
#include "io.h"
#include "shared.h"
#include "buttons.h"

/*
===============================================================================
  CONTROL TASK (Core 1) — 500 Hz
  - Reads buttons (debounced), consumes command queue from Web
  - Runs valve/pump state machine for FWD/REV/BEAT
  - Samples ADC (vent/atr), converts counts→mmHg (local linear fit)
  - Writes atomics: pwm, valve, vent_mmHg, atr_mmHg, loopMs
  Concurrency:
    • Normal: this task writes pwm/valve and hardware pins.
    • Calibration override active (G.overrideOutputs && now < overrideUntilMs):
        - Do NOT write hardware pins
        - Do NOT update G.pwm / G.valve (so SSE/UI retain manual values)
===============================================================================
*/

// UNO-derived linear conversion (for continuity with existing UI display).
static inline float countsToMMHg(uint16_t c) { return 0.4766825f * c - 204.409f; }

enum { MODE_FWD = 0, MODE_REV = 1, MODE_BEAT = 2 };
static inline int nextMode(int m){ return (m==MODE_FWD)?MODE_REV:(m==MODE_REV)?MODE_BEAT:MODE_FWD; }
static inline const char* modeName(int m){
  switch(m){ case MODE_FWD: return "FWD"; case MODE_REV: return "REV"; case MODE_BEAT: return "BEAT"; default: return "?"; }
}

enum Phase { PH_RAMP_UP, PH_HOLD, PH_RAMP_DOWN, PH_DEAD };

static inline int pwmFromPowerPct(int pct){
  if (pct < 10) pct = 10;
  if (pct > 100) pct = 100;
  const float x = pct * (1.0f/100.0f);
  int pwm = (int)roundf(PWM_FLOOR + x * (PWM_MAX - PWM_FLOOR));
  if (pwm < PWM_FLOOR) pwm = PWM_FLOOR;
  if (pwm > PWM_MAX)   pwm = PWM_MAX;
  return pwm;
}

static void applyCmdInline(const Cmd& cmd) {
  switch (cmd.type) {
    case CMD_TOGGLE_PAUSE: {
      int p = G.paused.load(std::memory_order_relaxed);
      p = p ? 0 : 1; G.paused.store(p, std::memory_order_relaxed);
      Serial.printf("[CTRL] (inline) TOGGLE_PAUSE -> paused=%d\n", p);
    } break;
    case CMD_SET_POWER_PCT: {
      int pct = cmd.iarg; if (pct < 10) pct = 10; if (pct > 100) pct = 100;
      G.powerPct.store(pct, std::memory_order_relaxed);
      Serial.printf("[CTRL] (inline) SET_POWER_PCT -> %d%%\n", pct);
    } break;
    case CMD_SET_MODE: {
      int m = cmd.iarg; if (m < MODE_FWD || m > MODE_BEAT) m = MODE_FWD;
      G.mode.store(m, std::memory_order_relaxed);
      Serial.printf("[CTRL] (inline) SET_MODE -> %s(%d)\n", modeName(m), m);
    } break;
    case CMD_SET_BPM: {
      float v = cmd.farg; if (v < 0.5f) v = 0.5f; if (v > 60.0f) v = 60.0f;
      G.bpm.store(v, std::memory_order_relaxed);
      Serial.printf("[CTRL] (inline) SET_BPM -> %.1f\n", v);
    } break;
  }
}

static void taskControl(void*) {
  const TickType_t period = pdMS_TO_TICKS(1000 / CONTROL_HZ);
  TickType_t wake = xTaskGetTickCount();

  pinMode(PIN_LED, OUTPUT);
  Serial.printf("[CTRL] running on core %d\n", xPortGetCoreID());

  bool led = false;
  uint32_t lastBlinkMs = millis();

  // Loop timing EMA
  float loopMsEMA = 1000.0f / CONTROL_HZ;
  uint32_t lastLoopStartUs = micros();

  // Status throttle
  uint32_t lastPrintMs = millis();

  // Buttons (A=Pause, B=Power ±10% short/long)
  uint32_t bPressMs = 0; bool bLongFired = false; const uint32_t LONG_MS = 500;
  bool comboActive = false, comboFired = false;
  uint32_t lastAPressMs = 0, lastBPressMs = 0;
  const uint32_t COMBO_WIN_MS = 120;

  // Control state
  Phase phase = PH_RAMP_UP;
  bool  dirForward  = true;   // actual valve drive direction
  bool  wantForward = true;   // desired from mode
  uint32_t phaseStartMs = millis();
  float pwmCmd    = 0.0f;
  int   targetPWM = pwmFromPowerPct(G.powerPct.load());

  float beat_hold_ms = 0.0f;
  int   lastModeSeen  = G.mode.load();
  int   lastPowerSeen = G.powerPct.load();

  auto recomputeBeatBudget = [&](){
    float bpm = G.bpm.load();
    if (bpm < 0.5f) bpm = 0.5f;
    if (bpm > 60.0f) bpm = 60.0f;
    const float Th_ms = (60.0f / bpm) * 1000.0f * 0.5f;

    const float tUp_ms   = (targetPWM - PWM_FLOOR) / PWM_SLOPE_UP   * 1000.0f;
    const float tDown_ms = (targetPWM - PWM_FLOOR) / PWM_SLOPE_DOWN * 1000.0f;

    float tHold = Th_ms - (tUp_ms + tDown_ms + (float)DEADTIME_MS);
    if (tHold < 0) tHold = 0;
    beat_hold_ms = tHold;
  };

  for (;;) {
    // Loop timing
    const uint32_t nowUs = micros();
    const float dtMs = (nowUs - lastLoopStartUs) * 0.001f;
    lastLoopStartUs = nowUs;
    loopMsEMA = loopMsEMA * 0.9f + dtMs * 0.1f;
    G.loopMs.store(loopMsEMA, std::memory_order_relaxed);

    // Buttons
    BtnState ev{}; buttons_read_debounced(ev);
    const uint32_t nowMs = millis();
    if (ev.aPressEdge) lastAPressMs = nowMs;
    if (ev.bPressEdge) lastBPressMs = nowMs;

    // A+B => cycle mode
    if (!comboActive) {
      if ((ev.aPressed && (nowMs - lastBPressMs) <= COMBO_WIN_MS) ||
          (ev.bPressed && (nowMs - lastAPressMs) <= COMBO_WIN_MS) ||
          (ev.aPressed && ev.bPressed)) {
        comboActive = true; comboFired = false; bLongFired = false;
      }
    }
    if (comboActive) {
      if (!ev.aPressed && !ev.bPressed && !comboFired) {
        const int cur = G.mode.load(std::memory_order_relaxed);
        const int nm  = nextMode(cur);
        Cmd c{ CMD_SET_MODE, nm, 0.0f };
        if (!shared_cmd_post(c)) applyCmdInline(c);
        comboFired  = true;
        comboActive = false;
      }
    } else {
      // Singles
      if (ev.aPressEdge) {
        Cmd c{ CMD_TOGGLE_PAUSE, 0, 0.0f };
        if (!shared_cmd_post(c)) applyCmdInline(c);
      }
      if (ev.bPressEdge) { bPressMs = nowMs; bLongFired = false; }
      if (ev.bPressed && !bLongFired) {
        if (nowMs - bPressMs >= LONG_MS) {
          int cur = G.powerPct.load(std::memory_order_relaxed);
          int target = cur - 10; if (target < 10) target = 100;
          Cmd c{ CMD_SET_POWER_PCT, target, 0.0f };
          if (!shared_cmd_post(c)) applyCmdInline(c);
          bLongFired = true;
        }
      }
      if (ev.bReleaseEdge && !bLongFired) {
        int cur = G.powerPct.load(std::memory_order_relaxed);
        int target = cur + 10; if (target > 100) target = 10;
        Cmd c{ CMD_SET_POWER_PCT, target, 0.0f };
        if (!shared_cmd_post(c)) applyCmdInline(c);
      }
    }

    // Consume queued commands from Web
    Cmd cmd; bool modeChanged=false, powerChanged=false, bpmChanged=false;
    while (shared_cmdq() && xQueueReceive(shared_cmdq(), &cmd, 0) == pdTRUE) {
      switch (cmd.type) {
        case CMD_TOGGLE_PAUSE: {
          int p = G.paused.load(std::memory_order_relaxed);
          p = p ? 0 : 1; G.paused.store(p, std::memory_order_relaxed);
          Serial.printf("[CTRL] cmd: TOGGLE_PAUSE -> paused=%d\n", p);
        } break;
        case CMD_SET_POWER_PCT: {
          int pct = cmd.iarg; if (pct < 10) pct = 10; if (pct > 100) pct = 100;
          G.powerPct.store(pct, std::memory_order_relaxed);
          Serial.printf("[CTRL] cmd: SET_POWER_PCT -> %d%%\n", pct);
          powerChanged = true;
        } break;
        case CMD_SET_MODE: {
          int m = cmd.iarg; if (m < MODE_FWD || m > MODE_BEAT) m = MODE_FWD;
          G.mode.store(m, std::memory_order_relaxed);
          Serial.printf("[CTRL] cmd: SET_MODE -> %s(%d)\n", modeName(m), m);
          modeChanged = true;
        } break;
        case CMD_SET_BPM: {
          float v = cmd.farg; if (v < 0.5f) v = 0.5f; if (v > 60.0f) v = 60.0f;
          G.bpm.store(v, std::memory_order_relaxed);
          Serial.printf("[CTRL] cmd: SET_BPM -> %.1f\n", v);
          bpmChanged = true;
        } break;
      }
    }

    // State machine updates
    const bool paused = G.paused.load() != 0;
    const int  mode   = G.mode.load();
    const int  power  = G.powerPct.load();

    // Calibration override window check
    const uint32_t nowMs2 = nowMs;
    const bool overrideActive =
      (G.overrideOutputs.load(std::memory_order_relaxed) != 0) &&
      (nowMs2 < G.overrideUntilMs.load(std::memory_order_relaxed));

    bool  wantForward = (mode != MODE_REV);

    if (powerChanged || power != lastPowerSeen) {
      targetPWM = paused ? 0 : pwmFromPowerPct(power);
      if (mode == MODE_BEAT) recomputeBeatBudget();
      lastPowerSeen = power;
    }

    if (modeChanged || mode != lastModeSeen) {
      lastModeSeen = mode;
      if (mode == MODE_BEAT) {
        dirForward = true; phase = PH_RAMP_UP; phaseStartMs = nowMs;
        targetPWM = paused ? 0 : pwmFromPowerPct(power);
        recomputeBeatBudget();
      } else {
        phase = (wantForward == dirForward) ? PH_RAMP_UP : PH_RAMP_DOWN;
        phaseStartMs = nowMs;
      }
    }

    if (bpmChanged) {
      if (mode == MODE_BEAT) recomputeBeatBudget();
    }

    if (paused) {
      pwmCmd = 0.0f;
      if (!overrideActive) {
        G.pwm.store(0, std::memory_order_relaxed);
        G.valve.store(0, std::memory_order_relaxed);
      }
    #if ENABLE_OUTPUTS
      if (!overrideActive) {
        io_pwm_write(0);
        io_valve_write(false);
      }
    #endif
    } else {
      if (mode == MODE_FWD || mode == MODE_REV) {
        if ((wantForward != dirForward) && (phase != PH_RAMP_DOWN && phase != PH_DEAD)) {
          phase = PH_RAMP_DOWN; phaseStartMs = nowMs;
        }
        switch (phase) {
          case PH_RAMP_UP:
            if (pwmCmd < PWM_FLOOR) pwmCmd = PWM_FLOOR;
            pwmCmd += PWM_SLOPE_UP * (dtMs * 0.001f);
            if (pwmCmd >= targetPWM) { pwmCmd = (float)targetPWM; phase = PH_HOLD; phaseStartMs = nowMs; }
            break;
          case PH_HOLD:
            if (pwmCmd < targetPWM - 0.5f)      phase = PH_RAMP_UP;
            else if (pwmCmd > targetPWM + 0.5f) phase = PH_RAMP_DOWN;
            break;
          case PH_RAMP_DOWN:
            pwmCmd -= PWM_SLOPE_DOWN * (dtMs * 0.001f);
            if (pwmCmd <= PWM_FLOOR) { pwmCmd = 0.0f; phase = PH_DEAD; phaseStartMs = nowMs; }
            break;
          case PH_DEAD:
            if ((nowMs - phaseStartMs) >= DEADTIME_MS) {
              dirForward = wantForward; phase = PH_RAMP_UP; phaseStartMs = nowMs;
            }
            break;
        }
      } else { // MODE_BEAT
        switch (phase) {
          case PH_RAMP_UP:
            if (pwmCmd < PWM_FLOOR) pwmCmd = PWM_FLOOR;
            pwmCmd += PWM_SLOPE_UP * (dtMs * 0.001f);
            if (pwmCmd >= targetPWM) { pwmCmd = (float)targetPWM; phase = PH_HOLD; phaseStartMs = nowMs; }
            break;
          case PH_HOLD:
            if ((nowMs - phaseStartMs) >= (uint32_t)beat_hold_ms) { phase = PH_RAMP_DOWN; phaseStartMs = nowMs; }
            break;
          case PH_RAMP_DOWN:
            pwmCmd -= PWM_SLOPE_DOWN * (dtMs * 0.001f);
            if (pwmCmd <= PWM_FLOOR) { pwmCmd = 0.0f; phase = PH_DEAD; phaseStartMs = nowMs; }
            break;
          case PH_DEAD:
            if ((nowMs - phaseStartMs) >= DEADTIME_MS) {
              dirForward = !dirForward; phase = PH_RAMP_UP; phaseStartMs = nowMs;
              recomputeBeatBudget();
            }
            break;
        }
      }

      const int outPWM = (int)constrain(lroundf(pwmCmd), 0, PWM_MAX);

      if (!overrideActive) {
        G.pwm.store((unsigned)outPWM, std::memory_order_relaxed);
        G.valve.store(dirForward ? 1u : 0u, std::memory_order_relaxed);
      }

    #if ENABLE_OUTPUTS
      if (!overrideActive) {
        io_valve_write(dirForward);
        io_pwm_write((uint8_t)outPWM);
      }
    #endif
    }

    // Sensors → convert here (browser later shows calibrated stream)
    const uint16_t rVent = io_adc_read_vent();
    const uint16_t rAtr  = io_adc_read_atr();
    G.vent_mmHg.store(countsToMMHg(rVent), std::memory_order_relaxed);
    G.atr_mmHg .store(countsToMMHg(rAtr),  std::memory_order_relaxed);

    // Status
    if (millis() - lastPrintMs >= STATUS_MS) {
      lastPrintMs = nowMs;
      const float hz = 1000.0f / (loopMsEMA > 0 ? loopMsEMA : 1);
      const char* ph = (phase==PH_RAMP_UP)?"UP":(phase==PH_HOLD)?"HOLD":(phase==PH_RAMP_DOWN)?"DOWN":"DEAD";
      Serial.printf("[CTRL] Hz=%.1f paused=%d mode=%s override=%d pwm=%3u tgt=%3d bpm=%.1f hold=%.0fms phase=%s\n",
                    hz, G.paused.load(), modeName(lastModeSeen),
                    (int)((G.overrideOutputs.load()!=0) && (millis()<G.overrideUntilMs.load())),
                    G.pwm.load(), targetPWM, G.bpm.load(), beat_hold_ms, ph);
    }

    // 1 Hz heartbeat LED
    if (millis() - lastBlinkMs >= 1000) { lastBlinkMs = nowMs; led = !led; digitalWrite(PIN_LED, led ? HIGH : LOW); }

    vTaskDelayUntil(&wake, period);
  }
}

void control_start_task() {
  xTaskCreatePinnedToCore(taskControl, "control", 6144, nullptr, 4, nullptr, CONTROL_CORE);
}
