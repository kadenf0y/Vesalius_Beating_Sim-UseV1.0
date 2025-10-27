#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "app_config.h"
#include "shared.h"
#include "web_cal.h"

/*
===============================================================================
  Web subsystem
  - SoftAP bring-up
  - Routes: UI ("/"), SSE "/stream", control APIs, calibration "/cal"
  - SSE packer @ ~30 Hz sends calibrated mmHg + flow + state
  Concurrency: reads atomics; posts commands via queue (inline fallback).
===============================================================================
*/

// AP credentials
static const char* AP_SSID = "VesaliusSimUse";
static const char* AP_PASS = "Vesal1us";

// HTTP + SSE
static AsyncWebServer server(80);
static AsyncEventSource sse("/stream");

// Hooks for calibration page (raw readers unchanged; not used for overrides)
static float readAtrRawOnce_impl(){ return G.atr_mmHg.load(std::memory_order_relaxed); }
static float readVentRawOnce_impl(){ return G.vent_mmHg.load(std::memory_order_relaxed); }
static int   getValveDir_impl(){
  const unsigned v = G.valve.load(std::memory_order_relaxed);
  return (v > 0) ? 1 : 0;
}
static void  setValveDir_impl(int dir){
  const unsigned v = (dir > 0) ? 1u : 0u;
  G.valve.store(v, std::memory_order_relaxed);
}
static void  setPwmRaw_impl(uint8_t pwm){ G.pwm.store((unsigned)pwm, std::memory_order_relaxed); }

// UI page (inlined; fixed ranges; responsive)
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">
<title>VSU ESP32 Monitor</title>
<style>
:root{--bg:#0f1317;--panel:#151a20;--ink:#e6edf3;--muted:#9aa7b2;--grid:#26303a;
      --cyan:#0ea5e9;--red:#ef4444;--amber:#f59e0b;--green:#22c55e;--violet:#a78bfa;}
*{box-sizing:border-box} html,body{height:100%;overflow:hidden}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.5 system-ui,Segoe UI,Roboto,Arial}
.top{display:flex;gap:8px;align-items:center;padding:6px 10px;background:#0c1014;border-bottom:1px solid var(--grid)}
.pill{font-size:12px;color:var(--muted);border:1px solid var(--grid);border-radius:999px;padding:2px 8px}
.main{display:grid;gap:12px;padding:10px;grid-template-columns:minmax(320px,1fr) minmax(96px,120px) minmax(260px,300px);
      height:calc(100vh - 40px);max-width:100vw}
.leftgrid,.midgrid,.rightgrid{display:grid;grid-template-rows:repeat(5,minmax(60px,1fr));gap:10px;min-height:0}
.strip{display:grid;grid-template-rows:auto 1fr auto;gap:0;background:#151a20;border:1px solid var(--grid);
       border-radius:12px;overflow:hidden;min-height:0}
.stripHead{display:flex;justify-content:space-between;align-items:center;padding:4px 8px;color:var(--muted);
          font-size:11px;border-bottom:1px solid var(--grid);background:#0c1116}
.legend{display:flex;gap:10px;align-items:center;font-size:11px;color:var(--muted);
       padding:4px 8px;border-top:1px solid var(--grid);background:#0c1116}
.sw{width:14px;height:3px;border-radius:2px;background:var(--muted)}
.canvasWrap{height:100%;min-height:0} canvas{display:block;width:100%;height:100%;background:#0b0f13}
.bigNum{display:flex;align-items:center;justify-content:center;height:100%;background:#0b0f13;border:1px solid var(--grid);
       border-radius:10px;min-height:0}
.numStack{display:flex;flex-direction:column;align-items:center;line-height:1}
.numStack .val{font-weight:900;letter-spacing:.02em;font-size:clamp(18px,3.0vh,28px)}
.numStack .unit{font-size:11px;color:var(--muted);margin-top:4px}
.t-cyan .val{color:#0ea5e9} .t-red .val{color:#ef4444} .t-amber .val{color:#f59e0b} .t-green .val{color:#22c55e} .t-violet .val{color:#a78bfa}
.group{height:100%;display:flex;flex-direction:column;justify-content:center;align-items:stretch;border:1px solid var(--grid);
      border-radius:10px;background:#0b0f13;padding:8px;min-height:0;overflow:hidden}
.group h3{margin:0 0 6px 0;font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}
.hint{font-size:11px;color:var(--muted)} .row{display:flex;gap:8px;align-items:center}
.bp{display:inline-flex;align-items:baseline;gap:8px;white-space:nowrap}.bp .main{font-weight:900;font-size:34px}
.pp{width:100%;padding:12px 10px;border-radius:10px;font-weight:800;border:1px solid var(--grid);cursor:pointer}
.pp.go{background:#1f8f44;border-color:#2aa456}.pp.stop{background:#a93636;border-color:#be4a4a}
.seg{display:flex;border:1px solid var(--grid);border-radius:10px;overflow:hidden}
.seg button{flex:1;border:0;padding:8px 10px;background:#0f1317;color:#9aa7b2;cursor:pointer}
.seg button.active{background:#142033;color:#e6edf3}.seg button:not(:last-child){border-right:1px solid var(--grid)}
.stepper{display:grid;grid-template-rows:auto 1fr auto;align-items:center;justify-items:center;gap:6px;min-height:0}
.stepper .btn{width:100%;padding:8px;border:1px solid var(--grid);border-radius:8px;background:#0f1317;color:#e6edf3;cursor:pointer}
.stepper .readout{display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%;width:100%;min-height:0;
                 border:1px solid var(--grid);border-radius:10px;background:#0f1317;padding:8px}
.stepper .val{font-weight:900;font-size:22px}.twoCol{display:grid;grid-template-columns:1fr 1fr;gap:8px;height:100%}
.dirTile{display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%}
.dirTile .arrow{font-size:28px;line-height:1;color:#22c55e}
@media (max-width:900px){.main{grid-template-columns:1fr}}
</style>
</head>
<body>
  <div class="top" id="topbar">
    <span class="pill">SSE: <b id="sse">INIT</b></span>
    <span class="pill">FPS: <b id="fps">—</b></span>
    <span class="pill">Hz: <b id="hz">—</b></span>
    <span class="pill">Loop: <b id="loop">—</b></span>
    <span class="pill">IP: <b id="ip">—</b></span>
  </div>

  <div class="main" id="main">
    <div class="leftgrid" id="leftgrid">
      <div class="strip"><div class="stripHead"><span>Atrium</span><span>mmHg</span></div>
        <div class="canvasWrap"><canvas id="cv-atr"></canvas></div>
        <div class="legend"><span class="sw" style="background:#0ea5e9"></span> −5 … 205 mmHg</div>
      </div>
      <div class="strip"><div class="stripHead"><span>Ventricle</span><span>mmHg</span></div>
        <div class="canvasWrap"><canvas id="cv-vent"></canvas></div>
        <div class="legend"><span class="sw" style="background:#ef4444"></span> −5 … 205 mmHg</div>
      </div>
      <div class="strip"><div class="stripHead"><span>Flow</span><span>L/min</span></div>
        <div class="canvasWrap"><canvas id="cv-flow"></canvas></div>
        <div class="legend"><span class="sw" style="background:#f59e0b"></span> 0 … 10 L/min</div>
      </div>
      <div class="strip"><div class="stripHead"><span>Valve Dir</span><span>0…1</span></div>
        <div class="canvasWrap"><canvas id="cv-valve"></canvas></div>
        <div class="legend"><span class="sw" style="background:#22c55e"></span> 0 (Reverse) … 1 (Forward)</div>
      </div>
      <div class="strip"><div class="stripHead"><span>Pump PWM</span><span>counts</span></div>
        <div class="canvasWrap"><canvas id="cv-power"></canvas></div>
        <div class="legend"><span class="sw" style="background:#a78bfa"></span> floor … 255</div>
      </div>
    </div>

    <div class="midgrid" id="midgrid">
      <div class="bigNum t-cyan"><div class="numStack"><span class="val" id="n-atr">—</span><span class="unit">mmHg</span></div></div>
      <div class="bigNum t-red"><div class="numStack"><span class="val" id="n-vent">—</span><span class="unit">mmHg</span></div></div>
      <div class="bigNum t-amber"><div class="numStack"><span class="val" id="n-flow">—</span><span class="unit">L/min</span></div></div>
      <div class="bigNum"><div class="dirTile"><div>Ventricle</div><div class="arrow" id="n-valve-arrow">-</div><div>Atrium</div></div></div>
      <div class="bigNum t-violet"><div class="numStack"><span class="val" id="n-power">—</span><span class="unit">PWM</span></div></div>
    </div>

    <div class="rightgrid" id="rightgrid">
      <div class="group"><h3>Blood Pressure</h3>
        <div class="bigNum"><div class="numStack"><span class="val" id="bp">—/—</span><span class="unit">mmHg</span></div></div>
        <div class="hint">SYS/DIA over last beat cycle</div>
      </div>
      <div class="group"><h3>Run</h3><button id="btnPause" class="pp go">Play</button></div>
      <div class="group"><h3>Control</h3>
        <div class="hint" style="margin-bottom:6px">Space: Play/Pause • 1/2/3: Mode • ←/→: Power ±1 • ↑/↓: BPM ±1</div>
        <a class="row" href="/cal" style="text-decoration:none;color:#8ab4f8;background:#0f1a26;border:1px solid #1b3550;border-radius:8px;padding:8px;justify-content:center">
          ⚙️ Calibration &amp; Sensor Trim
        </a>
      </div>
      <div class="group"><h3>Mode</h3>
        <div class="seg" id="modeSeg"><button data-m="0">Forward</button><button data-m="1">Reverse</button><button data-m="2">Beat</button></div>
      </div>
      <div class="group"><h3>Setpoints</h3>
        <div class="twoCol">
          <div class="stepper" id="stpPower"><button class="btn" data-dir="up">▲</button>
            <div class="readout"><div class="val" id="powerLabel" contenteditable="true" spellcheck="false">—%</div><div class="sub">Power 10–100%</div></div>
            <button class="btn" data-dir="down">▼</button>
          </div>
          <div class="stepper" id="stpBpm"><button class="btn" data-dir="up">▲</button>
            <div class="readout"><div class="val" id="bpmLabel" contenteditable="true" spellcheck="false">—</div><div class="sub">BPM 1–60</div></div>
            <button class="btn" data-dir="down">▼</button>
          </div>
        </div>
      </div>
    </div>
  </div>

<script>
// Helpers
const $ = (id)=>document.getElementById(id); $('ip').textContent = location.host || '192.168.4.1';

// Fit layout (simple height lock)
function fitLayout(){
  const top = document.getElementById('topbar');
  const main = document.getElementById('main');
  const topH = top.getBoundingClientRect().height || 0;
  const availMainH = Math.max(300, window.innerHeight - topH - 10);
  main.style.height = availMainH + 'px';
}
addEventListener('resize', fitLayout);

// Canvas helpers
function fitCanvas(canvas, ctx){
  const cssW = Math.max(1, canvas.clientWidth|0);
  const cssH = Math.max(1, canvas.clientHeight|0);
  if (canvas.width !== cssW || canvas.height !== cssH){ canvas.width=cssW; canvas.height=cssH; }
  ctx.setTransform(1,0,0,1,0,0);
}
function drawGrid(ctx, w, h, v=5, hlines=4){
  ctx.save(); ctx.lineWidth=1; ctx.strokeStyle=getComputedStyle(document.documentElement).getPropertyValue('--grid').trim()||'#26303a';
  ctx.beginPath(); const gx=w/v, gy=h/hlines;
  for(let x=0;x<=w;x+=gx){ ctx.moveTo(x+0.5,0); ctx.lineTo(x+0.5,h); }
  for(let y=0;y<=h;y+=gy){ ctx.moveTo(0,y+0.5); ctx.lineTo(w,y+0.5); }
  ctx.stroke(); ctx.restore();
}

const WINDOW_SEC = 5.0, PWM_FLOOR = 165;
function makeStrip(id, cfg){
  const cv = $(id), ctx = cv.getContext('2d');
  const st = { min:cfg.min, max:cfg.max, color:cfg.color }; const buf=[];
  function push(v){ const t=performance.now()/1000; buf.push({t,v}); while(buf.length && t - buf[0].t > WINDOW_SEC) buf.shift(); render(); }
  function render(){
    fitCanvas(cv, ctx); const W=cv.width,H=cv.height; ctx.clearRect(0,0,W,H); drawGrid(ctx,W,H,5,4);
    if (buf.length<2) return;
    const Y=(val)=>{ const vv=Math.max(st.min,Math.min(st.max,val)); return H - ((vv-st.min)/(st.max-st.min))*H; };
    const X=(t)=> ((t%WINDOW_SEC)/WINDOW_SEC)*W;
    ctx.lineWidth=2; ctx.strokeStyle=st.color; const now=performance.now()/1000;
    for (let i=1;i<buf.length;i++){ const a=buf[i-1], b=buf[i]; const xa=X(a.t), xb=X(b.t); if (xb<xa && (xa-xb)>(W*0.2)) continue;
      let alpha = 1 - ((now - b.t)/WINDOW_SEC); if (alpha<=0) continue; alpha*=alpha; ctx.globalAlpha=alpha;
      ctx.beginPath(); ctx.moveTo(xa, Y(a.v)); ctx.lineTo(xb, Y(b.v)); ctx.stroke();
    }
    ctx.globalAlpha=1;
  }
  addEventListener('resize', render);
  return { push, render, buf, state: st };
}

const stripAtr   = makeStrip('cv-atr',   {min: -5,   max:205,  color:'#0ea5e9'});
const stripVent  = makeStrip('cv-vent',  {min: -5,   max:205,  color:'#ef4444'});
const stripFlow  = makeStrip('cv-flow',  {min:  0,   max:10,   color:'#f59e0b'}); // L/min
const stripValve = makeStrip('cv-valve', {min:  0,   max:1,    color:'#22c55e'}); // 0..1
const stripPower = makeStrip('cv-power', {min:  0,   max:255,  color:'#a78bfa'});

function setText(id,s){ $(id).textContent=s; }

function computeBP(){
  const s = computeBP._s || (computeBP._s = { prevDir:null, sysMax:0, diaMax:0, lastBPText:'—/—' });
  const v = stripVent.buf.at(-1)?.v; const valveRaw = stripValve.buf.at(-1)?.v;
  if (v == null || valveRaw == null){ setText('bp', s.lastBPText); return; }
  const modeBtn = document.querySelector('#modeSeg button.active'); const mode = modeBtn ? (Number(modeBtn.dataset.m) || 0) : 0;
  if (mode !== 2){ s.prevDir=null; s.sysMax=0; s.diaMax=0; s.lastBPText='NA/NA'; setText('bp', s.lastBPText); return; }
  const dir = (valveRaw > 0.5) ? 1 : 0; // window separation
  if (dir === 1){ if (v > s.sysMax) s.sysMax = v; } else { if (v > s.diaMax) s.diaMax = v; }
  if (s.prevDir === 1 && dir === 0){ s.lastBPText = `${s.sysMax.toFixed(1)}/${s.diaMax.toFixed(1)}`; setText('bp', s.lastBPText); s.sysMax=0; s.diaMax=0; }
  else setText('bp', s.lastBPText);
  s.prevDir = (s.prevDir == null) ? dir : dir;
}

// Controls (unchanged bindings omitted for brevity — same as previous build)

// SSE
const es = new EventSource('/stream'); let lastFrame=performance.now(), emaFPS=0;
es.onopen=()=>{$('sse').textContent='OPEN';}; es.onerror=()=>{$('sse').textContent='ERROR';};
es.onmessage=(ev)=>{
  const now=performance.now(), dt=now-lastFrame; lastFrame=now; const fps=1000/Math.max(1,dt);
  emaFPS = emaFPS ? (emaFPS*0.98 + fps*0.02) : fps; $('fps').textContent = Math.round(emaFPS);
  try{
    const d = JSON.parse(ev.data);
    stripAtr.push( Number(d.atr_mmHg)   || 0 );
    stripVent.push(Number(d.vent_mmHg)  || 0 );

    // Convert mL/min (SSE) → L/min (UI)
    const flowLpm = (Number(d.flow_ml_min)||0) / 1000;
    stripFlow.push(flowLpm);
    document.getElementById('n-flow').textContent = flowLpm.toFixed(2);

    // Valve: use unsigned 0/1; 0 -> '-', 1 -> '▲'
    const valveU = (Number(d.valve)||0) ? 1 : 0;
    stripValve.push(valveU);
    document.getElementById('n-valve-arrow').textContent = valveU ? '▲' : '-';

    const pwmRaw = (typeof d.pwm === 'number') ? d.pwm : PWM_FLOOR; stripPower.push(pwmRaw);
    document.getElementById('n-atr').textContent  = (Number(d.atr_mmHg)||0).toFixed(1);
    document.getElementById('n-vent').textContent = (Number(d.vent_mmHg)||0).toFixed(1);

    const loopMs = Number(d.loopMs)||0; if (loopMs>0){ $('hz').textContent=(1000/loopMs).toFixed(1); $('loop').textContent=loopMs.toFixed(2)+' ms'; }
    document.querySelectorAll('#modeSeg button').forEach(b=> b.classList.toggle('active', Number(b.dataset.m)===Number(d.mode||0)));

    computeBP();
  }catch(e){}
};

addEventListener('load', ()=>{ fitLayout(); stripAtr.render(); stripVent.render(); stripFlow.render(); stripValve.render(); stripPower.render(); });
</script>
</body></html>
)HTML";

// Helper to post or inline-apply (queue fallback)
static inline void postOrInline(const Cmd& c){
  if (!shared_cmd_post(c)) {
    switch(c.type){
      case CMD_TOGGLE_PAUSE: {
        int p=G.paused.load(std::memory_order_relaxed);
        G.paused.store(p?0:1,std::memory_order_relaxed);
      } break;
      case CMD_SET_POWER_PCT: {
        int pct=c.iarg; if(pct<10)pct=10; if(pct>100)pct=100;
        G.powerPct.store(pct,std::memory_order_relaxed);
      } break;
      case CMD_SET_MODE: {
        int m=c.iarg; if(m<0||m>2)m=0;
        G.mode.store(m,std::memory_order_relaxed);
      } break;
      case CMD_SET_BPM: {
        float b=c.farg; if(b<0.5f)b=0.5f; if(b>60.0f)b=60.0f;
        G.bpm.store(b,std::memory_order_relaxed);
      } break;
    }
  }
}

// SSE sender (~30 Hz), streams CALIBRATED pressures/flow
static void sse_task(void*) {
  const TickType_t period = pdMS_TO_TICKS(33);
  TickType_t wake = xTaskGetTickCount();
  static char buf[256];

  for(;;){
    const int    paused = G.paused.load(std::memory_order_relaxed);
    const int    mode   = G.mode.load(std::memory_order_relaxed);
    const int    power  = G.powerPct.load(std::memory_order_relaxed);
    const unsigned pwm  = G.pwm.load(std::memory_order_relaxed);
    const unsigned valv = G.valve.load(std::memory_order_relaxed);
    const float  bpm    = G.bpm.load(std::memory_order_relaxed);
    const float  loop   = G.loopMs.load(std::memory_order_relaxed);

    const float  vent = scale_vent(G.vent_mmHg.load(std::memory_order_relaxed));
    const float  atr  = scale_atrium(G.atr_mmHg.load(std::memory_order_relaxed));
    const float  flow = G.flow_ml_min.load(std::memory_order_relaxed);

    const int n = snprintf(buf, sizeof(buf),
      "{\"paused\":%d,\"mode\":%d,\"powerPct\":%d,"
      "\"pwm\":%u,\"valve\":%u,"
      "\"vent_mmHg\":%.1f,\"atr_mmHg\":%.1f,\"flow_ml_min\":%.1f,"
      "\"bpm\":%.1f,\"loopMs\":%.2f}",
      paused, mode, power, pwm, valv, vent, atr, flow, bpm, loop);

    if (n>0) sse.send(buf, "message", millis());
    vTaskDelayUntil(&wake, period);
  }
}

void web_start() {
  // SoftAP
  WiFi.mode(WIFI_AP);
  const bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  const IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WEB] AP %s (%s)\n", ok?"started":"FAILED", ip.toString().c_str());

  // Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* req){
    req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  server.on("/api/toggle", HTTP_GET, [](AsyncWebServerRequest* req){
    Cmd c{ CMD_TOGGLE_PAUSE, 0, 0.0f }; postOrInline(c); req->send(204);
  });

  server.on("/api/mode", HTTP_GET, [](AsyncWebServerRequest* req){
    if (req->hasParam("m")) { int m = req->getParam("m")->value().toInt(); Cmd c{ CMD_SET_MODE, m, 0.0f }; postOrInline(c); }
    req->send(204);
  });

  server.on("/api/power", HTTP_GET, [](AsyncWebServerRequest* req){
    if (req->hasParam("pct")) { int pct = req->getParam("pct")->value().toInt(); if (pct<10)pct=10; if(pct>100)pct=100; Cmd c{ CMD_SET_POWER_PCT, pct, 0.0f }; postOrInline(c); }
    req->send(204);
  });

  server.on("/api/bpm", HTTP_GET, [](AsyncWebServerRequest* req){
    if (req->hasParam("b")) { float b = req->getParam("b")->value().toFloat(); Cmd c{ CMD_SET_BPM, 0, b }; postOrInline(c); }
    req->send(204);
  });

  sse.onConnect([](AsyncEventSourceClient* client){
    if (client->lastId()) { Serial.printf("[WEB] SSE client reconnected, id: %u\n", client->lastId()); }
    else { Serial.println("[WEB] SSE client connected"); }
    client->send(": hello\n\n");
  });
  server.addHandler(&sse);

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

  // Calibration hooks + routes
  web_cal_set_hooks({ readAtrRawOnce_impl, readVentRawOnce_impl, getValveDir_impl, setValveDir_impl, setPwmRaw_impl });
  web_cal_register_routes(server);

  server.begin();

  // SSE loop
  xTaskCreatePinnedToCore(sse_task, "sse", 4096, nullptr, 3, nullptr, WEB_CORE);
}
