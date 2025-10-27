#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "web.h"
#include "web_cal.h"
#include "shared.h"
#include "app_config.h"
#include "io.h"

// ==============================
//  Ownership / Concurrency doc
//  ----------------------------
//  Core 0 (this file):
//    • Creates SoftAP, HTTP routes, SSE task @ 60 Hz.
//    • Posts commands to Core 1 via queue (shared_post).
//    • Reads Shared atomics for SSE snapshots.
//  Core 1:
//    • Control loop updates atomics, executes commands.
// ==============================

static AsyncWebServer server(kHttpPort);
static AsyncEventSource sse("/stream");

// ---- Minimal dark main UI (inline; fixed ranges; DPR-safe plotting) ----
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset="utf-8">
<title>SimUse</title><meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 body{background:#0b0f13;color:#e6edf3;font:14px/1.4 system-ui;margin:0}
 .wrap{max-width:1100px;margin:0 auto;padding:10px}
 h1{font-size:18px;margin:10px 0}
 .grid{display:grid;grid-template-columns:2fr 1fr;gap:10px}
 .card{border:1px solid #243042;border-radius:12px;background:#0e141b;padding:10px}
 .row{display:flex;gap:10px;align-items:center;margin:6px 0}
 button,input,select{background:#12181f;border:1px solid #283241;color:#e6edf3;border-radius:8px;padding:6px 10px}
 canvas{width:100%;height:120px;background:#0b0f13;border-radius:8px}
 .tile{display:flex;flex-direction:column;align-items:center;justify-content:center;height:80px;border:1px solid #243042;border-radius:10px;background:#0b0f13}
 .big{font-weight:800;font-size:28px}
 .muted{color:#9aa7b2;font-size:12px}
 .two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
 .arrow{font-size:28px;color:#22c55e;line-height:1}
 a{color:#8ab4f8;text-decoration:none}
</style>
<div class="wrap">
  <h1>SimUse ESP32 <span class="muted">• <a href="/cal">Calibration</a></span></h1>
  <div class="grid">
    <div class="card">
      <div class="row muted">Live signals (fixed ranges)</div>
      <canvas id="atr"></canvas>
      <canvas id="vent"></canvas>
      <canvas id="flow"></canvas>
      <canvas id="valv"></canvas>
      <canvas id="pwm"></canvas>
    </div>
    <div class="card">
      <div class="two">
        <div class="tile"><div class="big" id="nAtr">—</div><div class="muted">Atrium mmHg</div></div>
        <div class="tile"><div class="big" id="nVent">—</div><div class="muted">Ventricle mmHg</div></div>
        <div class="tile"><div class="big" id="nFlow">—</div><div class="muted">Flow L/min</div></div>
        <div class="tile"><div class="arrow" id="nValve">—</div><div class="muted">Valve (▲=1, ▼=0)</div></div>
        <div class="tile"><div class="big" id="nPwm">—</div><div class="muted">PWM (0–255)</div></div>
        <div class="tile"><div class="big" id="nBpm">—</div><div class="muted">BPM</div></div>
      </div>
      <hr style="border-color:#243042">
      <div class="row"><label>Mode</label>
        <select id="mode"><option value="0">Forward</option><option value="1">Reverse</option><option value="2">Beat</option></select>
      </div>
      <div class="row"><label>PWM</label>
        <input id="pwmIn" type="number" min="0" max="255" value="0">
        <button onclick="setPwm(+(document.getElementById('pwmIn').value||0))">Set</button>
        <button onclick="nudge(5)">+5</button>
        <button onclick="nudge(-5)">−5</button>
      </div>
      <div class="row"><label>BPM</label>
        <input id="bpmIn" type="number" min="1" max="60" value="30">
        <button onclick="setBpm(+(document.getElementById('bpmIn').value||0))">Set</button>
      </div>
      <div class="row">
        <button onclick="toggle()">Play/Pause</button>
      </div>
    </div>
  </div>
</div>
<script>
const UI = {
  atr:{min:-5,max:205,color:'#0ea5e9'},
  vent:{min:-5,max:205,color:'#ef4444'},
  flow:{min:0,max:7.5,color:'#f59e0b'},
  valve:{min:0,max:1,color:'#22c55e'},
  pwm:{min:0,max:256,color:'#a78bfa'}
};

function strip(canvas, cfg){
  const ctx = canvas.getContext('2d'); const buf=[];
  function push(v){
    const t=performance.now()/1000; buf.push({t,v});
    while(buf.length && t-buf[0].t>5) buf.shift();
    render();
  }
  function render(){
    const w=canvas.clientWidth|0, h=canvas.clientHeight|0;
    if (canvas.width!==w||canvas.height!==h){ canvas.width=w; canvas.height=h; }
    ctx.clearRect(0,0,w,h); ctx.strokeStyle=cfg.color; ctx.lineWidth=2;
    const X=t=> ((t%5)/5)*w;
    const Y=v=> h - (Math.max(cfg.min, Math.min(cfg.max,v)) - cfg.min)/(cfg.max-cfg.min)*h;
    for(let i=1;i<buf.length;i++){
      const a=buf[i-1], b=buf[i]; const xa=X(a.t), xb=X(b.t);
      ctx.globalAlpha = Math.max(0,1-((performance.now()/1000-b.t)/5));
      ctx.beginPath(); ctx.moveTo(xa,Y(a.v)); ctx.lineTo(xb,Y(b.v)); ctx.stroke();
    }
    ctx.globalAlpha=1;
  }
  addEventListener('resize', render);
  return {push, render};
}
const sAtr=strip(document.getElementById('atr'), UI.atr);
const sVent=strip(document.getElementById('vent'), UI.vent);
const sFlow=strip(document.getElementById('flow'), UI.flow);
const sValv=strip(document.getElementById('valv'), UI.valve);
const sPwm= strip(document.getElementById('pwm'),  UI.pwm);

function setTxt(id,v,fix){ document.getElementById(id).textContent = (v==null?'—':(fix!=null?Number(v).toFixed(fix):v)); }

function setPwm(v){ fetch('/api/pwm?duty='+Math.max(0,Math.min(255,v))); }
function setBpm(v){ v=Math.max(1,Math.min(60,v)); fetch('/api/bpm?b='+v); }
function toggle(){ fetch('/api/toggle'); }
function nudge(d){ const el=document.getElementById('pwmIn'); let v=(+el.value||0)+d; if(v>255)v=0; if(v<0)v=255; el.value=v; setPwm(v); }
document.getElementById('mode').addEventListener('change', e=> fetch('/api/mode?m='+Number(e.target.value||0)));

const es = new EventSource('/stream');
es.onmessage = ev=>{
  const d = JSON.parse(ev.data);
  sAtr.push(d.atr_mmHg); sVent.push(d.vent_mmHg); sFlow.push(d.flow_L_min);
  sValv.push(d.valve?1:0); sPwm.push(d.pwm);
  setTxt('nAtr', d.atr_mmHg|0); setTxt('nVent', d.vent_mmHg|0); setTxt('nFlow', d.flow_L_min,2);
  setTxt('nPwm', d.pwm); setTxt('nBpm', d.bpm);
  document.getElementById('nValve').textContent = d.valve? '▲' : '▼';
  // mirror controls (no lockout)
  document.getElementById('pwmIn').value = d.pwm;
  document.getElementById('bpmIn').value = d.bpm;
  document.getElementById('mode').value = d.mode;
};
</script>
)HTML";

// ---- NVS helpers for cal ----
#include <Preferences.h>
static bool nvs_save_all(bool atr,bool vent,bool flow){
  Preferences p; if (!p.begin("cal", false)) return false;
  if (atr){  p.putFloat("atr_m",  G.atr_m.load());  p.putFloat("atr_b",  G.atr_b.load()); }
  if (vent){ p.putFloat("ven_m",  G.vent_m.load()); p.putFloat("ven_b",  G.vent_b.load()); }
  if (flow){ p.putFloat("flo_m",  G.flow_m.load()); p.putFloat("flo_b",  G.flow_b.load()); }
  p.end(); return true;
}
static bool nvs_load_all(){
  Preferences p; if (!p.begin("cal", true)) return false;
  G.atr_m.store(p.getFloat("atr_m",  G.atr_m.load()));
  G.atr_b.store(p.getFloat("atr_b",  G.atr_b.load()));
  G.vent_m.store(p.getFloat("ven_m",  G.vent_m.load()));
  G.vent_b.store(p.getFloat("ven_b",  G.vent_b.load()));
  G.flow_m.store(p.getFloat("flo_m",  G.flow_m.load()));
  G.flow_b.store(p.getFloat("flo_b",  G.flow_b.load()));
  p.end(); return true;
}
static void nvs_defaults_all(){
  G.atr_m.store(CAL_ATR_DEFAULT.m);   G.atr_b.store(CAL_ATR_DEFAULT.b);
  G.vent_m.store(CAL_VENT_DEFAULT.m); G.vent_b.store(CAL_VENT_DEFAULT.b);
  G.flow_m.store(CAL_FLOW_DEFAULT.m); G.flow_b.store(CAL_FLOW_DEFAULT.b);
}

// ---- SSE task @ 60 Hz on Core 0 ----
static void sse_task(void*){
  const TickType_t per = pdMS_TO_TICKS(1000/SSE_HZ);
  TickType_t wake = xTaskGetTickCount();
  static char buf[512];
  for(;;){
    int mode  = G.mode.load();
    int paused= G.paused.load(); if (paused==2) paused=1; // present "pending" as paused
    int pwm   = G.pwmSet.load();
    int valve = G.valve.load();
    int bpm   = G.bpm.load();
    float atr = G.atr_mmHg.load();
    float ven = G.vent_mmHg.load();
    float fl  = G.flow_L_min.load();
    float loop= G.loopMs.load();

    int n = snprintf(buf, sizeof(buf),
      "{\"mode\":%d,\"paused\":%d,\"pwm\":%d,\"valve\":%d,\"bpm\":%d,\"loopMs\":%.3f,"
      "\"atr_mmHg\":%.3f,\"vent_mmHg\":%.3f,\"flow_L_min\":%.3f,"
      "\"atr_raw\":%d,\"vent_raw\":%d,\"flow_hz\":%.3f,"
      "\"cal\":{\"atr_m\":%.6f,\"atr_b\":%.6f,\"vent_m\":%.6f,\"vent_b\":%.6f,\"flow_m\":%.6f,\"flow_b\":%.6f}}",
      mode, paused, pwm, valve, bpm, loop,
      atr, ven, fl,
      G.atr_raw.load(), G.vent_raw.load(), G.flow_hz.load(),
      G.atr_m.load(), G.atr_b.load(), G.vent_m.load(), G.vent_b.load(), G.flow_m.load(), G.flow_b.load());
    if (n>0) sse.send(buf, "message", millis());
    vTaskDelayUntil(&wake, per);
  }
}

// ---- Route helpers (Core 0 posts commands) ----
static void post_or_inline(const Cmd& c){
  if (!shared_post(c)){
    // emergency inline adjust: update atomics as if Core 1 had consumed them
    if (c.t==CMD_TOGGLE){
      int p=G.paused.load(); G.paused.store(p?0:1);
    } else if (c.t==CMD_SET_PWM){
      int v=c.i; if(v<0)v=0; if(v>255)v=255; G.pwmSet.store(v);
    } else if (c.t==CMD_SET_BPM){
      int b=c.i; if(b<1)b=1; if(b>60)b=60; G.bpm.store(b);
    } else if (c.t==CMD_SET_MODE){
      int m=c.i; if(m<MODE_FWD||m>MODE_BEAT)m=MODE_FWD; G.mode.store(m);
    }
  }
}

// ---- Calibration hooks wiring ----
static void get_cals(float& am,float& ab,float& vm,float& vb,float& fm,float& fb){
  am=G.atr_m.load(); ab=G.atr_b.load(); vm=G.vent_m.load(); vb=G.vent_b.load(); fm=G.flow_m.load(); fb=G.flow_b.load();
}
static void set_cals(float am,float ab,float vm,float vb,float fm,float fb){
  G.atr_m.store(am); G.atr_b.store(ab); G.vent_m.store(vm); G.vent_b.store(vb); G.flow_m.store(fm); G.flow_b.store(fb);
}
static int  read_atr_raw(){ return G.atr_raw.load(); }
static int  read_vent_raw(){ return G.vent_raw.load(); }
static float read_flow_hz(){ return G.flow_hz.load(); }
static void write_pwm_raw(uint8_t d){ G.pwmSet.store(d); G.overrideOutputs.store(1); G.overrideUntilMs.store(millis()+3000); io_write_pwm(d); }
static void write_valve_raw(uint8_t v){ v=v?1:0; G.valve.store(v); G.overrideOutputs.store(1); G.overrideUntilMs.store(millis()+3000); io_write_valve(v); }

void web_start(){
  // SoftAP open (no password)
  WiFi.mode(WIFI_AP);
  WiFi.softAP(kApSsid);
  Serial.printf("[WEB] AP up: %s  IP=%s\n", kApSsid, WiFi.softAPIP().toString().c_str());

  // Pages
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ r->send_P(200, "text/html", INDEX_HTML); });

  // Control APIs (GET)
  server.on("/api/pwm", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("duty")){
      int v=r->getParam("duty")->value().toInt();
      post_or_inline({CMD_SET_PWM, v});
    }
    r->send(204);
  });
  server.on("/api/bpm", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("b")){
      int v=r->getParam("b")->value().toInt();
      post_or_inline({CMD_SET_BPM, v});
    }
    r->send(204);
  });
  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest* r){
    if (r->hasParam("m")){
      int v=r->getParam("m")->value().toInt();
      post_or_inline({CMD_SET_MODE, v});
    }
    r->send(204);
  });
  server.on("/api/toggle", HTTP_GET, [](AsyncWebServerRequest* r){
    post_or_inline({CMD_TOGGLE,0}); r->send(204);
  });

  // SSE
  sse.onConnect([](AsyncEventSourceClient* c){ c->send(": ok\n\n"); });
  server.addHandler(&sse);

  // Calibration sub-router
  CalHooks hooks{
    read_atr_raw, read_vent_raw, read_flow_hz,
    write_pwm_raw, write_valve_raw,
    get_cals, set_cals, nvs_save_all, nvs_load_all, nvs_defaults_all
  };
  web_cal_register(server, hooks);

  // Start
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.begin();

  // Streamer task on Core 0
  xTaskCreatePinnedToCore(sse_task, "sse", 4096, nullptr, 3, nullptr, CORE_WEB);
}
