#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
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

// ---- UI (inline HTML served from flash) ----
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SimUse — Live</title>
<style>
  :root{--bg:#0f1317;--panel:#151a20;--ink:#e6edf3;--muted:#9aa7b2;--grid:#26303a;
         --cyan:#0ea5e9;--red:#ef4444;--amber:#f59e0b;--green:#22c55e;--violet:#a78bfa}
  *{box-sizing:border-box} html,body{height:100%;margin:0;background:var(--bg);color:var(--ink);font:14px/1.5 system-ui,Segoe UI,Roboto,Arial}
  .top{display:flex;gap:8px;align-items:center;padding:8px 12px;background:#0c1014;border-bottom:1px solid var(--grid)}
  .pill{font-size:12px;color:var(--muted);border:1px solid var(--grid);border-radius:999px;padding:4px 10px}
  .main{display:grid;gap:12px;padding:10px;grid-template-columns:minmax(420px,1fr) minmax(120px,160px) minmax(120px,160px);height:calc(100vh - 56px)}
  .col{display:grid;grid-template-rows:repeat(5,1fr);gap:10px;min-height:0}
  .panel{background:var(--panel);border:1px solid var(--grid);border-radius:12px;overflow:hidden;display:flex;flex-direction:column}
  .panelHeader{padding:6px 10px;color:var(--muted);font-size:12px;border-bottom:1px solid var(--grid);display:flex;justify-content:space-between}
  .panelBody{flex:1;min-height:0;padding:8px}
  canvas{width:100%;height:100%;display:block;background:#0b0f13;border-radius:6px}
  .big{display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%;min-height:0}
  .big .val{font-weight:900;font-size:28px}
  .muted{color:var(--muted);font-size:12px}
  .val-cyan{color:var(--cyan)}.val-red{color:var(--red)}.val-amber{color:var(--amber)}.val-violet{color:var(--violet)}.val-green{color:var(--green)}
  .controls{display:flex;flex-direction:column;align-items:stretch;gap:10px;padding:12px}
  /* each control row stacks label above the controls for clearer alignment */
  .controls .row{display:flex;flex-direction:column;align-items:stretch;gap:6px}
  .controls .row label{width:auto;flex:0 0 auto;margin:0 0 6px 0}
  .controls .row .ctrl{flex:1;display:flex;align-items:center;gap:8px;flex-wrap:wrap}
  .seg{display:flex;border:1px solid var(--grid);border-radius:10px;overflow:hidden;flex-direction:column;width:100%}
  .seg button{padding:6px;border:0;background:transparent;color:var(--muted);cursor:pointer;width:100%;text-align:center}
  .seg button.active{background:#142033;color:var(--ink)}
  .row{display:flex;gap:8px;align-items:center;justify-content:space-between}
  .btn{padding:6px 8px;border-radius:8px;border:1px solid var(--grid);background:#0f1317;color:var(--ink);cursor:pointer;min-width:40px}
  .btn-play{background:#0f3f11;color:#eaffef;border-color:#244f1d}
  .btn-pause{background:#3b0f0f;color:#ffecec;border-color:#5a1616}
  @media (max-width:900px){ .main{grid-template-columns:1fr} }
</style>
</head><body>
  <div class="top">
    <div class="pill">SSE: <b id="sse">INIT</b></div>
    <div class="pill">IP: <b id="ip">—</b></div>
    <div style="flex:1"></div>
    <div class="pill">FPS: <b id="fps">—</b></div>
    <div class="pill">Loop: <b id="loop">—</b></div>
  </div>

  <div class="main">
    <div class="col">
      <div class="panel"><div class="panelHeader"><span>Atrium</span><span class="muted">mmHg</span></div>
        <div class="panelBody"><canvas id="cv-atr"></canvas></div></div>
      <div class="panel"><div class="panelHeader"><span>Ventricle</span><span class="muted">mmHg</span></div>
        <div class="panelBody"><canvas id="cv-vent"></canvas></div></div>
      <div class="panel"><div class="panelHeader"><span>Flow</span><span class="muted">L/min</span></div>
        <div class="panelBody"><canvas id="cv-flow"></canvas></div></div>
      <div class="panel"><div class="panelHeader"><span>Valve</span><span class="muted">0/1</span></div>
        <div class="panelBody"><canvas id="cv-valve"></canvas></div></div>
      <div class="panel"><div class="panelHeader"><span>PWM</span><span class="muted">0–255</span></div>
        <div class="panelBody"><canvas id="cv-pwm"></canvas></div></div>
    </div>

    <div class="col" style="grid-template-rows:repeat(5,auto)">
      <div class="panel big"><div class="big"><div class="val val-cyan" id="n-atr">—</div><div class="muted">Atrium mmHg</div></div></div>
      <div class="panel big"><div class="big"><div class="val val-red" id="n-vent">—</div><div class="muted">Ventricle mmHg</div></div></div>
      <div class="panel big"><div class="big"><div class="val val-amber" id="n-flow">—</div><div class="muted">Flow L/min</div></div></div>
      <div class="panel big"><div class="big"><div class="val val-green" id="n-valve">-</div><div class="muted">Valve ▲=1 ▼=0</div></div></div>
    <div class="panel big"><div class="big"><div class="val val-violet" id="n-pwm">—</div><div class="muted">POWER</div></div></div>
    </div>

    <div class="panel controls">
      <div style="width:100%"><button id="btnToggle" class="btn btn-play">Play</button></div>
      <div class="row" style="width:100%"><label class="muted">Mode</label>
        <div class="seg" id="modeSeg"><button data-m="0">Forward</button><button data-m="1">Reverse</button><button data-m="2">Beat</button></div>
      </div>
      <div class="row" style="width:100%"><label class="muted">POWER</label>
        <div class="ctrl">
          <div style="display:flex;flex-direction:row;align-items:center;gap:8px;flex-wrap:wrap">
            <div style="display:flex;flex-direction:column;align-items:center;gap:6px">
              <button id="btnPwmPlus" class="btn">+5</button>
              <input id="pwmIn" type="number" min="0" max="255" value="0" style="width:72px;text-align:center;padding:6px;border-radius:6px;border:1px solid var(--grid);background:#0f1317;color:var(--ink)">
              <button id="btnPwmMinus" class="btn">-5</button>
            </div>
            <div style="flex:1;min-width:8px"></div>
          </div>
        </div>
      </div>
      <div class="row" style="width:100%"><label class="muted">BPM</label>
        <div class="ctrl">
          <div style="display:flex;flex-direction:row;align-items:center;gap:8px;flex-wrap:wrap">
            <div style="display:flex;flex-direction:column;align-items:center;gap:6px">
              <button id="btnBpmPlus" class="btn">+5</button>
              <input id="bpmIn" type="number" min="1" max="60" value="30" style="width:72px;text-align:center;padding:6px;border-radius:6px;border:1px solid var(--grid);background:#0f1317;color:var(--ink)">
              <button id="btnBpmMinus" class="btn">-5</button>
            </div>
            <div style="flex:1;min-width:8px"></div>
          </div>
        </div>
      </div>
      <div style="margin-top:6px"><a href="/cal" class="btn" style="text-decoration:none;display:inline-block">Calibration</a></div>
    </div>
  </div>

<script>
// small helpers
const $ = id => document.getElementById(id);
try{ if($('ip')) $('ip').textContent = location.host || '192.168.4.1'; }catch(e){}

function fitCanvas(c, ctx){ const w=c.clientWidth|0, h=c.clientHeight|0; if(c.width!==w||c.height!==h){ c.width=w; c.height=h; } ctx.setTransform(1,0,0,1,0,0); }
function drawGrid(ctx,W,H){ ctx.save(); ctx.strokeStyle=getComputedStyle(document.documentElement).getPropertyValue('--grid').trim()||'#26303a'; ctx.lineWidth=1; ctx.beginPath(); for(let x=0;x<=W;x+=W/5){ ctx.moveTo(x+0.5,0); ctx.lineTo(x+0.5,H); } for(let y=0;y<=H;y+=H/4){ ctx.moveTo(0,y+0.5); ctx.lineTo(W,y+0.5); } ctx.stroke(); ctx.restore(); }

function makeStrip(id, cfg){ const c=$(id), ctx=c.getContext('2d'); const buf=[]; const WIN=5.0;
  let lastW=0, lastH=0;
  function push(v){ const t=performance.now()/1000; buf.push({t,v}); while(buf.length && (performance.now()/1000 - buf[0].t)>WIN) buf.shift(); render(); }
  function render(){ fitCanvas(c,ctx); const W=c.width,H=c.height;
    // if resized, clear and redraw background grid
    if (W!==lastW || H!==lastH){ ctx.clearRect(0,0,W,H); drawGrid(ctx,W,H); lastW=W; lastH=H; }
    if(buf.length<1) return;
    // We intentionally do NOT clear the data layer each frame so new samples overwrite old pixels.
    // Map time to X using modulo so data advances left->right and wraps back to 0.
    const X = t => ((t % WIN) / WIN) * W;
    const Y = v => H - ((Math.max(cfg.min,Math.min(cfg.max,v)) - cfg.min)/(cfg.max-cfg.min)) * H;
    ctx.lineWidth = 2; ctx.strokeStyle = cfg.color; ctx.globalAlpha = 1; ctx.lineJoin='round'; ctx.lineCap='round';
    // draw a background-colored vertical bar centered at the newest sample X so it moves with the data
    const bg = (getComputedStyle(document.documentElement).getPropertyValue('--bg')||'#0b0f13').trim();
    const last = buf[buf.length-1];
    const xb_latest = X(last.t);
  const barW = 100;
    ctx.fillStyle = bg;
    // draw the bar as one or two integer-aligned rects to avoid per-pixel gaps and anti-aliasing
    const start = Math.round(xb_latest - Math.floor(barW/2));
    const s = ((start % W) + W) % W; // wrapped start
    if (s + barW <= W) {
      // single rect
      ctx.fillRect(s, 0, barW, H);
    } else {
      // wrapped: two rects
      const w1 = W - s;
      const w2 = barW - w1;
      ctx.fillRect(s, 0, w1, H);
      ctx.fillRect(0, 0, w2, H);
    }

    // draw segments, but don't connect across wrap boundaries; when wrapping, draw a small dot at wrapped X
    for(let i=1;i<buf.length;i++){
      const a = buf[i-1], b = buf[i]; const ta = a.t % WIN, tb = b.t % WIN;
      const xa = X(a.t), xb = X(b.t);
      if (tb < ta){ // wrapped: draw a small dot at xb (data drawn on top of the background bar)
        const r = Math.max(1, Math.min(3, Math.round(Math.max(1, Math.min(3, Math.floor(W/200))))));
        ctx.fillStyle = cfg.color;
        ctx.beginPath(); ctx.arc(xb, Y(b.v), r, 0, Math.PI*2); ctx.fill();
      } else {
        ctx.beginPath(); ctx.strokeStyle = cfg.color; ctx.moveTo(xa, Y(a.v)); ctx.lineTo(xb, Y(b.v)); ctx.stroke();
      }
    }
  }
  window.addEventListener('resize', ()=>{ lastW=0; lastH=0; render(); }); return { push, render }; }

const sAtr = makeStrip('cv-atr',{min:-5,max:205,color:'#0ea5e9'});
const sVent= makeStrip('cv-vent',{min:-5,max:205,color:'#ef4444'});
const sFlow= makeStrip('cv-flow',{min:0,max:7.5,color:'#f59e0b'});
const sValve=makeStrip('cv-valve',{min:0,max:1,color:'#22c55e'});
const sPwm = makeStrip('cv-pwm',{min:0,max:255,color:'#a78bfa'});

function setNum(id,v,fix){ if(!$(id)) return; $(id).textContent = (v==null)?'—':(fix!=null?Number(v).toFixed(fix):v); }

// Controls
function post(url){ fetch(url).catch(()=>{}); }
if($('btnToggle')) $('btnToggle').addEventListener('click',()=>post('/api/toggle'));
// Apply buttons removed; inputs auto-apply via +5/-5 or on change handlers
if($('btnPwmPlus')) $('btnPwmPlus').addEventListener('click',()=>{ adjustPwm(5); });
if($('btnPwmMinus')) $('btnPwmMinus').addEventListener('click',()=>{ adjustPwm(-5); });
if($('btnBpmPlus')) $('btnBpmPlus').addEventListener('click',()=>{ adjustBpm(5); });
if($('btnBpmMinus')) $('btnBpmMinus').addEventListener('click',()=>{ adjustBpm(-5); });
// auto-apply: also post when input values change
if($('pwmIn')) $('pwmIn').addEventListener('change', ()=>{ const v=Number($('pwmIn').value||0); post('/api/pwm?duty='+Math.max(0,Math.min(255,v))); });
if($('bpmIn')) $('bpmIn').addEventListener('change', ()=>{ const v=Number($('bpmIn').value||30); post('/api/bpm?b='+Math.max(1,Math.min(60,v))); });
document.querySelectorAll('#modeSeg button').forEach(b=> b.addEventListener('click', e=>{ post('/api/mode?m='+b.dataset.m); }));

function adjustPwm(d){ const el=$('pwmIn'); if(!el) return; let v=Number(el.value||0); v = Math.max(0, Math.min(255, v + d)); el.value = v; post('/api/pwm?duty='+v); }
function adjustBpm(d){ const el=$('bpmIn'); if(!el) return; let v=Number(el.value||0); v = Math.max(1, Math.min(60, v + d)); el.value = v; post('/api/bpm?b='+v); }

// SSE receiver — uses server keys: atr_mmHg, vent_mmHg, flow_L_min, pwmSet, pwm, valve, mode, bpm, loopMs
const es = new EventSource('/stream'); let last=performance.now(), ema=0;
es.onopen=()=>$('sse')? $('sse').textContent='OPEN' : null; es.onerror=()=>$('sse')? $('sse').textContent='ERR' : null;
es.onmessage=(ev)=>{ const now=performance.now(); const dt=now-last; last=now; const fps=1000/Math.max(1,dt); ema= ema? (ema*0.98 + fps*0.02) : fps; if($('fps')) $('fps').textContent=Math.round(ema);
  try{ const d=JSON.parse(ev.data);
  sAtr.push(Number(d.atr_mmHg)||0); sVent.push(Number(d.vent_mmHg)||0); sFlow.push(Number(d.flow_L_min)||0);
  sValve.push(d.valve?1:0); sPwm.push(Number(d.pwm)||0);
    setNum('n-atr', d.atr_mmHg, 1); setNum('n-vent', d.vent_mmHg, 1); setNum('n-flow', d.flow_L_min, 2); setNum('n-pwm', d.pwm,0);
    if($('n-valve')) $('n-valve').textContent = d.valve? '▲' : '▼';
    document.querySelectorAll('#modeSeg button').forEach(b=> b.classList.toggle('active', Number(b.dataset.m)===Number(d.mode||0)));
  if($('pwmIn')) $('pwmIn').value = d.pwmSet||0; if($('bpmIn')) $('bpmIn').value = d.bpm||0; if(d.loopMs && $('loop')) $('loop').textContent = Number(d.loopMs).toFixed(2)+' ms';
    if($('btnToggle')){
      const b=$('btnToggle'); if(Number(d.paused||0)===0){ b.textContent='Pause'; b.classList.remove('btn-play'); b.classList.add('btn-pause'); } else { b.textContent='Play'; b.classList.remove('btn-pause'); b.classList.add('btn-play'); }
    }
  }catch(e){}
};

window.addEventListener('load', ()=>{ sAtr.render(); sVent.render(); sFlow.render(); sValve.render(); sPwm.render(); });
</script>
</body></html>
)HTML";

// Small helper page to view the raw SSE stream with two modes:
// - Auto-scroll down (newest at bottom)
// - Newest-on-top (prepend messages)
static const char STREAM_VIEW_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Stream Viewer</title>
<style>body{font-family:system-ui,Segoe UI,Roboto,Arial;background:#0b0f13;color:#e6edf3;margin:0;padding:8px} .bar{display:flex;gap:8px;align-items:center;margin-bottom:8px}
button{padding:6px 8px;border-radius:6px;border:1px solid #26303a;background:#0f1317;color:#e6edf3}
#log{height:75vh;overflow:auto;border:1px solid #26303a;padding:8px;border-radius:6px;background:#071018;font-family:monospace;font-size:12px}
.entry{padding:6px;border-radius:6px;margin-bottom:6px;background:rgba(255,255,255,0.02);word-break:break-all}
</style>
</head><body>
  <div class="bar">
    <label style="font-size:13px">Mode:</label>
    <select id="mode"><option value="bottom">Auto-scroll bottom (newest at bottom)</option><option value="top">Newest on top (prepend)</option></select>
    <button id="clear">Clear</button>
    <div style="flex:1"></div>
    <div id="status">SSE: connecting...</div>
  </div>
  <div id="log"></div>

<script>
const log = document.getElementById('log'); const modeEl = document.getElementById('mode'); const status = document.getElementById('status');
const es = new EventSource('/stream');
es.onopen = ()=> status.textContent = 'SSE: open';
es.onerror = ()=> status.textContent = 'SSE: error';
es.onmessage = (ev)=>{ try{ const txt = ev.data; const el = document.createElement('div'); el.className='entry'; el.textContent = new Date().toLocaleTimeString() + '  ' + txt; if(modeEl.value==='top'){ log.insertBefore(el, log.firstChild); } else { log.appendChild(el); }
    // keep size reasonable
    while(log.children.length>500) { if(modeEl.value==='top') log.removeChild(log.lastChild); else log.removeChild(log.firstChild); }
    if(modeEl.value==='bottom'){ // auto-scroll to bottom
      log.scrollTop = log.scrollHeight;
    }
  }catch(e){}
};
document.getElementById('clear').addEventListener('click', ()=> log.innerHTML='');
</script>
</body></html>
)HTML";

// ---- SSE task @ 60 Hz on Core 0 ----
static void sse_task(void*){
  const TickType_t per = pdMS_TO_TICKS(1000/SSE_HZ);
  TickType_t wake = xTaskGetTickCount();
  static char buf[512];
  for(;;){
    int mode  = G.mode.load();
      int paused= G.paused.load(); if (paused==2) paused=1; // present "pending" as paused
      int pwmSet= G.pwmSet.load();
      int pwm   = G.pwmOut.load();
      int valve = G.valve.load();
      int bpm   = G.bpm.load();
      float atr = G.atr_mmHg.load();
      float ven = G.vent_mmHg.load();
      float fl  = G.flow_L_min.load();
      float loop= G.loopMs.load();

      int n = snprintf(buf, sizeof(buf),
        "{\"mode\":%d,\"paused\":%d,\"pwmSet\":%d,\"pwm\":%d,\"valve\":%d,\"bpm\":%d,\"loopMs\":%.3f,"
        "\"atr_mmHg\":%.3f,\"vent_mmHg\":%.3f,\"flow_L_min\":%.3f,"
        "\"atr_raw\":%d,\"vent_raw\":%d,\"flow_hz\":%.3f,"
        "\"cal\":{\"atr_m\":%.6f,\"atr_b\":%.6f,\"vent_m\":%.6f,\"vent_b\":%.6f,\"flow_m\":%.6f,\"flow_b\":%.6f}}",
        mode, paused, pwmSet, pwm, valve, bpm, loop,
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
// NVS helpers used by the calibration UI hooks
static bool nvs_save_all(bool atr,bool vent,bool flow){
  Preferences p; if(!p.begin("cal", false)) return false;
  if (atr){ p.putFloat("atr_m", G.atr_m.load()); p.putFloat("atr_b", G.atr_b.load()); }
  if (vent){ p.putFloat("ven_m", G.vent_m.load()); p.putFloat("ven_b", G.vent_b.load()); }
  if (flow){ p.putFloat("flo_m", G.flow_m.load()); p.putFloat("flo_b", G.flow_b.load()); }
  p.end(); return true;
}
static bool nvs_load_all(){
  Preferences p; if(!p.begin("cal", false)) return false;
  float am = p.getFloat("atr_m",  G.atr_m.load());
  float ab = p.getFloat("atr_b",  G.atr_b.load());
  float vm = p.getFloat("ven_m",  G.vent_m.load());
  float vb = p.getFloat("ven_b",  G.vent_b.load());
  float fm = p.getFloat("flo_m",  G.flow_m.load());
  float fb = p.getFloat("flo_b",  G.flow_b.load());
  p.end();
  G.atr_m.store(am);  G.atr_b.store(ab);
  G.vent_m.store(vm); G.vent_b.store(vb);
  G.flow_m.store(fm); G.flow_b.store(fb);
  return true;
}
static void nvs_defaults_all(){
  G.atr_m.store(CAL_ATR_DEFAULT.m); G.atr_b.store(CAL_ATR_DEFAULT.b);
  G.vent_m.store(CAL_VENT_DEFAULT.m); G.vent_b.store(CAL_VENT_DEFAULT.b);
  G.flow_m.store(CAL_FLOW_DEFAULT.m); G.flow_b.store(CAL_FLOW_DEFAULT.b);
}
static int  read_atr_raw(){ return G.atr_raw.load(); }
static int  read_vent_raw(){ return G.vent_raw.load(); }
static float read_flow_hz(){ return G.flow_hz.load(); }
static void write_pwm_raw(uint8_t d){ G.pwmSet.store(d); G.pwmOut.store(d); G.overrideOutputs.store(1); G.overrideUntilMs.store(millis()+3000); io_write_pwm(d); }
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

  // Stream viewer page - does not replace /stream (SSE) but provides a friendly UI at /stream/view
  server.on("/stream/view", HTTP_GET, [](AsyncWebServerRequest* r){ r->send_P(200, "text/html", STREAM_VIEW_HTML); });

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
