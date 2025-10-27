#include "control.h"
#include "shared.h"
#include "buttons.h"
#include "io.h"
#include "app_config.h"

static uint8_t clamp8(int v){ if(v<0) v=0; if(v>255) v=255; return (uint8_t)v; }

static inline bool override_active(){
  if (!G.overrideOutputs.load()) return false;
  return (millis() < G.overrideUntilMs.load());
}

static inline void refresh_override_gate(){
  G.overrideOutputs.store(1);
  G.overrideUntilMs.store(millis()+3000);
}

// Simple 10-sample moving-average ring for each pressure channel
struct MA {
  float buf[PRESS_SMOOTH_N]{};
  uint8_t idx=0, n=0;
  float sum=0;
  inline void push(float v){
    sum -= buf[idx];
    buf[idx]=v;
    sum += v;
    idx = (idx+1) % PRESS_SMOOTH_N;
    if (n < PRESS_SMOOTH_N) n++;
  }
  inline float mean() const { return n? (sum/n):0; }
};

// Move the main control loop into a FreeRTOS task so `control_start()`
// can return immediately and allow other subsystems (like web_start)
// to initialize. This pins the control loop to CORE_CONTROL.
static void control_task(void* /*arg*/){
  // Blink & queue ready
  pinMode(PIN_STATUS_LED, OUTPUT);

  // local state
  uint8_t pwm_out = 0;                   // current hardware PWM
  float pwm_out_f = 0.0f;               // fractional accumulator for smooth ramping
  uint8_t pwm_set = 0;                   // desired setpoint (snapshot of G.pwmSet)
  uint8_t valve_dir = VALVE_FWD;         // current hardware valve

  // Beat timing helpers
  auto compute_half_hold_ms = [&]()->uint32_t{
    int bpm = G.bpm.load(); if (bpm<1) bpm=1; if (bpm>60) bpm=60;
    uint32_t half_ms = (uint32_t)(60000.0f/(2.0f*bpm));
    // used = ramp_up + dead + ramp_down
    uint32_t used = RAMP_MS + DEAD_MS + RAMP_MS;
    return (half_ms>used)? (half_ms-used) : 0u;
  };

  // timing
  float loopEmaMs = 1000.0f/CONTROL_HZ;
  uint32_t lastUs = micros();
  const TickType_t period = pdMS_TO_TICKS(1000/CONTROL_HZ);
  TickType_t wake = xTaskGetTickCount();

  // Buttons
  BtnState bs{};
  uint32_t aRiseTs = 0, bRiseTs = 0;          // rise timestamps for chord detection
  uint32_t bPressMs = 0; bool bLongFired = false;
  static constexpr uint32_t CHORD_TOL_MS = 80; // tolerance for near-simultaneous A+B
  static uint8_t chord_gated = 0;

  // Sequencers
  uint8_t seq = 0;           // direction change seq: 0=idle,1=down,2=dead,3=flip,4=up
  uint32_t seqT = 0;

  // Beat substate
  uint8_t bstate = 0;        // 0=up,1=hold,2=down,3=dead1,4=dead2
  uint32_t bstateT = 0;
  uint32_t beatHoldMs = compute_half_hold_ms();

  // previous paused for edge detection
  uint8_t prevPaused = (uint8_t)G.paused.load();

  // helper lambdas
  auto setValveIfChanged = [&](uint8_t d){
    if (valve_dir != d){ valve_dir = d; io_write_valve(valve_dir); }
  };
  auto setPwmIfChanged = [&](uint8_t d){
    if (pwm_out != d){ pwm_out = d; pwm_out_f = (float)pwm_out; io_write_pwm(pwm_out); }
  };
  // rampToward: move pwm_out toward target in RAMP_MS across CONTROL_HZ ticks
  auto rampToward = [&](uint8_t target, bool allowRampUp=true){
    if (pwm_out == target) return;
    if (!allowRampUp && target > pwm_out) return; // don't ramp up when disallowed
    float ticks = (float)RAMP_MS / (1000.0f/CONTROL_HZ);
    if (ticks < 1.0f) ticks = 1.0f;
    // compute fractional step using the fractional accumulator so small steps accumulate over ticks
    float step = ((float)target - pwm_out_f) / ticks;
    pwm_out_f += step;
    // clamp accumulator to target to avoid overshoot
    if (step > 0.0f && pwm_out_f > (float)target) pwm_out_f = (float)target;
    if (step < 0.0f && pwm_out_f < (float)target) pwm_out_f = (float)target;
    int next = (int)roundf(pwm_out_f);
    if ((step>0 && next>target) || (step<0 && next<target)) next = target;
    pwm_out = clamp8(next);
    io_write_pwm(pwm_out);
  };
  auto forceOutputsOff = [&](){ pwm_out = 0; io_write_pwm(0); valve_dir = VALVE_FWD; io_write_valve(valve_dir); };

  for(;;){
    // timing
    uint32_t nowUs = micros();
    float dtMs = (nowUs-lastUs)*0.001f; lastUs = nowUs;
    loopEmaMs = loopEmaMs*0.9f + dtMs*0.1f;
    G.loopMs.store(loopEmaMs, std::memory_order_relaxed);

    // consume commands
    Cmd cmd;
    while (shared_cmdq() && xQueueReceive(shared_cmdq(), &cmd, 0) == pdTRUE){
      if (cmd.t == CMD_TOGGLE){
        int paused = G.paused.load();
        if (paused){
          // unpause -> immediate valve direction set and begin ramp up
          G.paused.store(0);
          // valve and ramp-up handled below on running path using need_dir/pwm_set
        } else {
          // request pending pause: finish ramp to zero then set paused
          G.paused.store(2);
        }
      } else if (cmd.t == CMD_SET_PWM){
        int v = cmd.i; if (v<0) v=0; if (v>255) v=255; G.pwmSet.store(v);
      } else if (cmd.t == CMD_SET_BPM){
        int b = cmd.i; if (b<1) b=1; if (b>60) b=60; G.bpm.store(b); beatHoldMs = compute_half_hold_ms();
      } else if (cmd.t == CMD_SET_MODE){
        int m = cmd.i; if (m<MODE_FWD||m>MODE_BEAT) m = MODE_FWD;
        // do not auto-unpause on mode change; just update mode
        G.mode.store(m);
        // recompute beat budget if needed
        if (m==MODE_BEAT) {
          beatHoldMs = compute_half_hold_ms();
          // reset beat substate so beat starts cleanly when running
          bstate = 0; bstateT = millis();
        }
        // if running, start direction-change sequence (or start beat immediately)
        if (G.paused.load()==0){
          if (m==MODE_BEAT){
            // ensure beat starts from a ramp-up
            bstate = 0; bstateT = millis();
          } else {
            // start seq to change valve safely for FWD<->REV
            seq = 1; seqT = millis();
          }
        }
      }
    }

    // buttons
    buttons_read(bs);
    if (bs.aRise) aRiseTs = millis();
    if (bs.bRise) bRiseTs = millis();

    bool chord_now = false;
    if (bs.aPressed && bs.bPressed){
      if (!chord_gated){
        if (aRiseTs && bRiseTs && (abs((int32_t)aRiseTs - (int32_t)bRiseTs) <= (int32_t)CHORD_TOL_MS)) chord_now = true;
        else if (aRiseTs==0 && bRiseTs==0) chord_now = true;
      }
    }
    if (chord_now && !chord_gated){
      chord_gated = 1;
      int m = G.mode.load(); m = (m==MODE_BEAT)?MODE_FWD:(m+1); G.mode.store(m);
      if (G.paused.load()==0){ seq = 1; seqT = millis(); }
    }
    if (!bs.aPressed || !bs.bPressed) {
      chord_gated = 0;
      if (bs.aRise){ Cmd c{CMD_TOGGLE,0}; shared_post(c); }
      if (bs.bRise){ bPressMs = millis(); bLongFired=false; }
      if (bs.bPressed && !bLongFired && (millis()-bPressMs)>=600){ int p = G.pwmSet.load(); p -= 5; if (p<0) p=255; G.pwmSet.store(p); bLongFired=true; }
      if (bs.bFall && !bLongFired){ int p = G.pwmSet.load(); p += 5; if (p>255) p=0; G.pwmSet.store(p); }
    }

    // outputs
    pwm_set = (uint8_t)G.pwmSet.load();
    uint8_t need_dir = (G.mode.load()==MODE_REV)?VALVE_REV:VALVE_FWD;

    bool isOverride = override_active();
    int paused = G.paused.load(); // 0=run,1=paused,2=pending

    // override gate: still enforce safety (pause) writes
    if (isOverride){
      if (paused==1 || paused==2) {
        forceOutputsOff();
      }
      // otherwise do not write outputs while override active
    } else {
      // normal operation allowed to write outputs
      // handle pending pause
      if (paused==2){
        // finish ramping to zero
        if (pwm_out>0){
          // decrement per-tick roughly
          int next = (int)pwm_out - (int)roundf(255.0f / (RAMP_MS/(1000.0f/CONTROL_HZ)));
          pwm_out = clamp8(next);
          io_write_pwm(pwm_out);
        } else {
          // reached zero: mark paused and force valve off
          G.paused.store(1);
          forceOutputsOff();
        }
      } else if (paused==1){
        // paused: enforce PWM=0 and valve=FWD
        forceOutputsOff();
      } else {
        // running
        // if user just unpaused (edge), ensure valve immediately set to need_dir
        if (prevPaused==1 && paused==0){ setValveIfChanged(need_dir); }

        if (seq != 0){
          // direction change seq: ramp down -> dead -> flip -> ramp up
          if (seq==1){ // ramp down
            rampToward(0, true);
            if (pwm_out==0 && (millis()-seqT)>=0){ seq=2; seqT=millis(); }
          } else if (seq==2){ // dead wait
            if (millis()-seqT >= DEAD_MS){ seq=3; }
          } else if (seq==3){ // flip
            setValveIfChanged(need_dir);
            seq = 4; seqT = millis();
          } else if (seq==4){ // ramp up
            rampToward(pwm_set, true);
            if (pwm_out==pwm_set) seq = 0;
          }
        } else if (G.mode.load()==MODE_BEAT){
          // beat mode state machine (per half-period)
          if (bstate==0){ // ramp up
            rampToward(pwm_set, true);
            if (pwm_out==pwm_set){ bstate=1; bstateT=millis(); }
          } else if (bstate==1){ // hold
            if (millis()-bstateT >= beatHoldMs){ bstate=2; }
          } else if (bstate==2){ // ramp down
            rampToward(0, true);
            if (pwm_out==0){ bstate=3; bstateT=millis(); }
          } else if (bstate==3){ // dead half 1
            if (millis()-bstateT >= (DEAD_MS/2)){ // flip in middle
              // use helper so writes and local state are consistent
              setValveIfChanged((valve_dir==VALVE_FWD)?VALVE_REV:VALVE_FWD);
              bstate = 4; bstateT = millis();
            }
          } else if (bstate==4){ // dead half 2
            if (millis()-bstateT >= (DEAD_MS/2)){ bstate = 0; }
          }
        } else {
          // steady FWD or REV
          setValveIfChanged(need_dir);
          rampToward(pwm_set, true);
        }
      }
    }

  // publish: expose both setpoint and actual hardware PWM
  G.valve.store(valve_dir);
  G.pwmSet.store(pwm_set);
  G.pwmOut.store(pwm_out);

    // ADC + smoothing
    int atr_r = io_read_atr(); int vent_r = io_read_vent();
    G.atr_raw.store(atr_r); G.vent_raw.store(vent_r);
    // push smoothing (reuse MA local instances)
    static MA atr_ma{}, vent_ma{}; atr_ma.push((float)atr_r); vent_ma.push((float)vent_r);
    float atr_cal = apply_cal(atr_ma.mean(), G.atr_m.load(), G.atr_b.load());
    float vent_cal = apply_cal(vent_ma.mean(), G.vent_m.load(), G.vent_b.load());
    G.atr_mmHg.store(atr_cal); G.vent_mmHg.store(vent_cal);

    // heartbeat LED ~1Hz
    static uint32_t ledT=0; static bool led=false;
    if (millis()-ledT >= 500){ ledT = millis(); led = !led; digitalWrite(PIN_STATUS_LED, led); }

    // update prevPaused and delay
    prevPaused = (uint8_t)G.paused.load();
    vTaskDelayUntil(&wake, period);
  }
}

void control_start(){
  // Create the control task pinned to CORE_CONTROL. Stack and priority chosen
  // to give the 600 Hz loop enough headroom; adjust if needed.
  const uint32_t stack = 8192; // bytes
  const UBaseType_t prio = 3;
  xTaskCreatePinnedToCore(control_task, "control", stack/sizeof(portSTACK_TYPE), nullptr, prio, nullptr, CORE_CONTROL);
}
