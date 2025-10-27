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
  uint8_t pwm_out = 0;
  uint8_t pwm_set = 0;         // desired setpoint (G.pwmSet)
  uint8_t valve_dir = VALVE_FWD;

  // Beat timing
  uint32_t phaseStartMs = millis();
  enum Phase { PH_RAMP_UP, PH_HOLD, PH_RAMP_DOWN, PH_DEAD } phase = PH_RAMP_UP;

  // helper: compute half-period & hold based on BPM and fixed ramps/deadtime
  auto compute_hold_ms = [&]()->uint32_t{
    int bpm = G.bpm.load(); if (bpm<1) bpm=1; if (bpm>60) bpm=60;
    uint32_t half_ms = (uint32_t)(60000.0f/(2.0f*bpm));
    uint32_t used = RAMP_MS + DEAD_MS + RAMP_MS;
    return (half_ms>used)? (half_ms-used) : 0u;
  };
  uint32_t beatHoldMs = compute_hold_ms();

  // loop timing EMA
  float loopEmaMs = 1000.0f/CONTROL_HZ;
  uint32_t lastUs = micros();

  // button handling
  BtnState bs{};
  uint32_t bPressMs=0; bool bLongFired=false;

  // ADC smoothing rings
  MA atr_ma{}, vent_ma{};

  // Task timing
  const TickType_t period = pdMS_TO_TICKS(1000/CONTROL_HZ);
  TickType_t wake = xTaskGetTickCount();

  for(;;){
    // --- loop timing
    uint32_t nowUs = micros();
    float dtMs = (nowUs-lastUs)*0.001f; lastUs=nowUs;
    loopEmaMs = loopEmaMs*0.9f + dtMs*0.1f;
    G.loopMs.store(loopEmaMs, std::memory_order_relaxed);

    // --- consume commands
    Cmd cmd;
    while (shared_cmdq() && xQueueReceive(shared_cmdq(), &cmd, 0) == pdTRUE){
      if (cmd.t==CMD_TOGGLE){
        // Pause only allowed when pwm==0 → if running, ramp to 0 then pause.
        int paused = G.paused.load();
        if (paused){ G.paused.store(0); phase = PH_RAMP_UP; phaseStartMs=millis(); }
        else{
          // request: ramp down to zero then pause (handled below)
          G.paused.store(2); // 2 = "pending pause after ramp-down"
        }
      } else if (cmd.t==CMD_SET_PWM){
        int v = cmd.i; if(v<0)v=0; if(v>255)v=255;
        G.pwmSet.store(v);
      } else if (cmd.t==CMD_SET_BPM){
        int b = cmd.i; if(b<1)b=1; if(b>60)b=60;
        G.bpm.store(b); beatHoldMs = compute_hold_ms();
      } else if (cmd.t==CMD_SET_MODE){
        int m = cmd.i; if(m<MODE_FWD||m>MODE_BEAT)m=MODE_FWD;
        G.mode.store(m);
        // reset phase on mode change
        phase = PH_RAMP_UP; phaseStartMs=millis();
      }
    }

    // --- buttons
    buttons_read(bs);
    // A: Play/Pause
    if (bs.aRise){
      Cmd c{CMD_TOGGLE,0}; shared_post(c);
    }
    // B: short (+5 wrap), long single fire (−5 wrap)
    if (bs.bRise){ bPressMs=millis(); bLongFired=false; }
    if (bs.bPressed && !bLongFired && (millis()-bPressMs)>=600){
      int p = G.pwmSet.load(); p -= 5; if (p<0) p=255; G.pwmSet.store(p); bLongFired=true;
    }
    if (bs.bFall && !bLongFired){
      int p = G.pwmSet.load(); p += 5; if (p>255) p=0; G.pwmSet.store(p);
    }
    // A+B chord: cycle mode
    if (bs.aPressed && bs.bPressed){
      static uint8_t gated=0;
      if (!gated){ gated=1; int m=G.mode.load(); m=(m==MODE_BEAT)?MODE_FWD:(m+1); G.mode.store(m); phase=PH_RAMP_UP; phaseStartMs=millis(); }
    }else{
      // release the gate when chord breaks
      static uint8_t& gated = *([]()->uint8_t*{ static uint8_t x=0; return &x; })();
      gated = 0;
    }

    // --- outputs sequencing (if not overridden)
    pwm_set   = (uint8_t)G.pwmSet.load();
    uint8_t need_dir = (G.mode.load()==MODE_REV)? VALVE_REV : VALVE_FWD;

    bool canWrite = !override_active();
    if (!canWrite){
      // Respect override: do not change outputs (also do not alter pwm_out/valve_dir state vars)
    }else{
      // Apply Play/Pause rule:
      int paused = G.paused.load(); // 0=run, 1=paused, 2=pending-pause
      if (paused==1){
        pwm_out = 0; io_write_pwm(0); io_write_valve(VALVE_FWD);
      }else{
        // immediate PWM setpoint changes ramp to new set (150 ms)
        // Compute per-tick step for 150 ms to move from current to target:
        auto rampToward = [&](uint8_t target){
          if (pwm_out==target) return;
          // step per tick = (target - pwm_out) / (RAMP_MS / (1000/CONTROL_HZ))
          float ticks = (float)RAMP_MS / (1000.0f/CONTROL_HZ);
          float step = (target - (int)pwm_out) / (ticks>1?ticks:1);
          int next = (int)roundf((float)pwm_out + step);
          if ((step>0 && next>target) || (step<0 && next<target)) next=target;
          pwm_out = clamp8(next);
        };

        // Direction change sequence (FWD↔REV):
        // ramp set→0 (150 ms) → dead 100 ms → flip valve → ramp 0→set (150 ms)
        static uint8_t seq=0; // 0=idle,1=down,2=dead,3=flip,4=up
        static uint32_t seqT=0;

        if (valve_dir != need_dir && seq==0){ seq=1; seqT=millis(); } // start sequence

        if (seq==1){
          rampToward(0);
          if (millis()-seqT >= RAMP_MS){ seq=2; seqT=millis(); }
        } else if (seq==2){
          if (millis()-seqT >= DEAD_MS){ seq=3; }
        } else if (seq==3){
          valve_dir = need_dir;
          io_write_valve(valve_dir);
          seq=4;
        } else if (seq==4){
          rampToward(pwm_set);
          if (pwm_out==pwm_set){ seq=0; }
        } else {
          // regular operation (no direction change)
          if (G.mode.load()==MODE_BEAT){
            // Beat: the above fixed sequence always runs; remaining time is hold
            static uint8_t bstate=0; // 0=up,1=hold,2=down,3=dead
            if (bstate==0){
              rampToward(pwm_set);
              if (pwm_out==pwm_set){ bstate=1; seqT=millis(); }
            }else if (bstate==1){
              if (millis()-seqT >= beatHoldMs){ bstate=2; }
            }else if (bstate==2){
              rampToward(0);
              if (pwm_out==0){ bstate=3; seqT=millis(); }
            }else if (bstate==3){
              if (millis()-seqT >= DEAD_MS){
                valve_dir = (valve_dir==VALVE_FWD)?VALVE_REV:VALVE_FWD;
                io_write_valve(valve_dir);
                bstate=0;
              }
            }
          }else{
            // FWD or REV steady: ensure correct direction and ramp to set
            valve_dir = need_dir;
            io_write_valve(valve_dir);
            rampToward(pwm_set);
          }
        }

        // Enforce pause pending: if pending, finish ramp to 0 then pause
        if (paused==2){
          if (pwm_out>0) {
            // keep ramping toward zero
            int next = (int)pwm_out - (int)roundf(255.0f / (RAMP_MS/(1000.0f/CONTROL_HZ)));
            pwm_out = clamp8(next);
          } else {
            G.paused.store(1); // now paused
          }
        }

        io_write_pwm(pwm_out);
      }
    }

    // publish current outputs (even if overridden, we publish last internal values)
    G.valve.store(valve_dir);
    G.pwmSet.store(pwm_set);

    // --- ADC read + smoothing + calibration
    int atr_r  = io_read_atr();
    int vent_r = io_read_vent();
    G.atr_raw.store(atr_r); G.vent_raw.store(vent_r);
    atr_ma.push((float)atr_r);
    vent_ma.push((float)vent_r);

    float atr_cal  = apply_cal(atr_ma.mean(),  G.atr_m.load(),  G.atr_b.load());
    float vent_cal = apply_cal(vent_ma.mean(), G.vent_m.load(), G.vent_b.load());
    G.atr_mmHg.store(atr_cal);
    G.vent_mmHg.store(vent_cal);

    // heartbeat LED ~ 1 Hz
    static uint32_t ledT=0; static bool led=0;
    if (millis()-ledT >= 500){ ledT=millis(); led=!led; digitalWrite(PIN_STATUS_LED, led); }

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
