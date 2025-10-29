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
// Live smoothing settings (default values)
static float g_smooth_atr = 0.15f;
static float g_smooth_vent = 0.15f;
static float g_smooth_flow = 0.20f;

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
  .btn-full{width:100%}
  /* Blood pressure numeric value should be plain white for contrast */
  #n-bp{color:#ffffff}
  /* POWER buttons use the PWM graph color (violet) */
  .btn-power{background:var(--violet);color:var(--ink);border-color:rgba(167,139,250,0.22)}
  .btn-power:hover{filter:brightness(1.05)}
  /* Mode buttons use the Valve graph color (green) */
  #modeSeg button{color:var(--green)}
  #modeSeg button.active{background:#0a2b12;color:var(--ink);border-color:rgba(34,197,94,0.18)}
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
    <div class="pill">JIT: <b id="jit">—</b></div>
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
  <div class="panel big"><div class="big"><div class="muted">Atrium</div><div class="val val-green" id="n-valve">-</div><div class="muted">Ventrical</div></div></div>
    <div class="panel big"><div class="big"><div class="val val-violet" id="n-pwm">—</div><div class="muted">POWER</div></div></div>
    </div>

    <div class="col" style="grid-template-rows:repeat(5,1fr)">
  <div class="panel big"><div class="big"><div class="muted">Blood Pressure</div><div class="val val-green" id="n-bp">N/A</div><div style="color:var(--ink);font-size:12px">mmHg</div><div class="muted">SYS/DIA</div></div></div>

      <div class="panel controls" style="grid-row:span 4">
  <div style="width:100%"><button id="btnToggle" class="btn btn-play btn-full">Play</button></div>
      <div class="row" style="width:100%"><label class="muted">Mode</label>
        <div class="seg" id="modeSeg"><button data-m="0">Forward</button><button data-m="1">Reverse</button><button data-m="2">Beat</button></div>
      </div>
      <div class="row" style="width:100%"><label class="muted">POWER</label>
        <div class="ctrl">
          <div style="display:flex;flex-direction:row;align-items:center;gap:8px;flex-wrap:wrap">
            <div style="display:flex;flex-direction:column;align-items:stretch;width:100%;gap:6px">
              <button id="btnPwmPlus" class="btn btn-full btn-power">+5</button>
              <input id="pwmIn" type="number" min="0" max="255" value="180" style="width:72px;align-self:center;text-align:center;padding:6px;border-radius:6px;border:1px solid var(--grid);background:#0f1317;color:var(--ink)">
              <button id="btnPwmMinus" class="btn btn-full btn-power">-5</button>
            </div>
            <div style="flex:1;min-width:8px"></div>
          </div>
        </div>
      </div>
      <div class="row" style="width:100%"><label class="muted">BPM</label>
        <div class="ctrl">
          <div style="display:flex;flex-direction:row;align-items:center;gap:8px;flex-wrap:wrap">
            <div style="display:flex;flex-direction:column;align-items:stretch;width:100%;gap:6px">
              <button id="btnBpmPlus" class="btn btn-full">+5</button>
              <input id="bpmIn" type="number" min="1" max="60" value="30" style="width:72px;align-self:center;text-align:center;padding:6px;border-radius:6px;border:1px solid var(--grid);background:#0f1317;color:var(--ink)">
              <button id="btnBpmMinus" class="btn btn-full">-5</button>
            </div>
            <div style="flex:1;min-width:8px"></div>
          </div>
        </div>
      </div>
  <div style="margin-top:6px">
    <label class="muted" style="display:block;margin-bottom:6px">Window: <b id="winLabel">5</b>s</label>
    <input id="winRange" type="range" min="5" max="60" step="1" value="5" style="width:100%">
  </div>
  <div style="margin-top:6px"><a href="/cal" class="btn btn-full" style="text-decoration:none;display:inline-block;text-align:center">Calibration</a></div>
  <div style="margin-top:6px"><a href="http://192.168.4.1/stream/view" class="btn btn-full" style="text-decoration:none;display:inline-block;text-align:center">Stream</a></div>
    </div>
  </div>


<script>
// small helpers
const $ = id => document.getElementById(id);
try{ if($('ip')) $('ip').textContent = location.host || '192.168.4.1'; }catch(e){}

// Create SSE connection early so the top-level SSE status (SSE: INIT → OPEN/ERR)
// updates even if a later script error would otherwise abort execution.
// If EventSource construction fails, provide a safe fallback object so code
// that assigns handlers or references `es` won't throw.
let es = null;
try{
  es = new EventSource('/stream');
  es.onopen = ()=>{ try{ if($('sse')) $('sse').textContent='OPEN'; }catch(e){} };
  es.onerror = ()=>{ try{ if($('sse')) $('sse').textContent='ERR'; }catch(e){} };
}catch(e){
  // fallback no-op EventSource-like object
  es = { onopen:null, onerror:null, onmessage:null, close:()=>{} };
  try{ if($('sse')) $('sse').textContent='ERR'; }catch(e){}
}

function fitCanvas(c, ctx){ const w=c.clientWidth|0, h=c.clientHeight|0; if(c.width!==w||c.height!==h){ c.width=w; c.height=h; } ctx.setTransform(1,0,0,1,0,0); }
function drawGrid(ctx,W,H){ /* grid disabled: no-op to remove background grid lines */ }

function makeStrip(id, cfg){ const c=$(id);
  // guard: if the canvas element is missing or getContext fails, return a no-op
  // strip so the rest of the UI can still function without throwing.
  if (!c){
    const noop = ()=>{};
    return { push: noop, render: noop, getSmoothed: ()=>null, setAlpha: noop };
  }
  const ctx = c.getContext && c.getContext('2d'); if(!ctx){ const noop = ()=>{}; return { push: noop, render: noop, getSmoothed: ()=>null, setAlpha: noop }; }
  const buf=[];
  // Window length (seconds) is dynamic and read from global `window.winSec` (default 5s).
  function getWin(){ return (typeof window.winSec === 'number' && window.winSec>0) ? window.winSec : 5.0; }
  let lastW=0, lastH=0;
  // optional smoothing: cfg.smoothAlpha in (0..1], higher -> more responsive, lower -> smoother
  let _prevSmoothed = null;
  let _alpha = (typeof cfg.smoothAlpha === 'number') ? Math.max(0, Math.min(1, cfg.smoothAlpha)) : 1.0;
  // push value into the strip. Optional second argument `tIn` can supply
  // a server-origin timestamp in seconds; if not provided we fall back to
  // the client's performance.now() time. Using server timestamps reduces
  // visual jitter caused by network/browser delivery delays.
  function push(v, tIn){ const t = (typeof tIn === 'number') ? tIn : (performance.now()/1000);
    let outV = v;
    if (_alpha < 1.0){
      if (_prevSmoothed === null) _prevSmoothed = outV;
      else _prevSmoothed = (_prevSmoothed * (1 - _alpha)) + (outV * _alpha);
      outV = _prevSmoothed;
    }
    buf.push({t,v:outV}); while(buf.length && (t - buf[0].t) > getWin()) buf.shift(); requestStripRender(); }
  function render(){ fitCanvas(c,ctx); const W=c.width,H=c.height;
    // if resized, clear and redraw background grid
    if (W!==lastW || H!==lastH){ ctx.clearRect(0,0,W,H); drawGrid(ctx,W,H); lastW=W; lastH=H; }
    if(buf.length<1) return;
    // Clear the drawing layer and redraw the entire buffer each frame. This guarantees
    // that only the current buffer is visible at any X coordinate and avoids artifacts
    // caused by incremental background overdraw or compositing seams.
    ctx.clearRect(0, 0, W, H);
    drawGrid(ctx, W, H);
  const win = getWin();
  const X = t => ((t % win) / win) * W;
    const Y = v => H - ((Math.max(cfg.min,Math.min(cfg.max,v)) - cfg.min)/(cfg.max-cfg.min)) * H;
    ctx.lineWidth = 2; ctx.strokeStyle = cfg.color; ctx.globalAlpha = 1; ctx.lineJoin='round'; ctx.lineCap='round';

    // draw segments. If cfg.step is true, render as horizontal steps with vertical transitions
    // We compute an "erase-ahead" region in pixels and fade samples that fall inside it.
  const last = buf[buf.length-1];
  const xb_latest = X(last.t);
  const gap = 8; // px gap in front of newest sample to keep visible
    const eraseFull = 50; // fully erased region after the gap
    const fadeEnd = 100;  // fade to opaque by this distance after the gap
    const totalW = fadeEnd;
    // helper: distance forward from xb_latest to x in pixels (0..W)
    function distAheadPx(x){ let d = x - xb_latest; if (d < 0) d += W; return d; }
  function alphaFromDist(d){ if (d <= gap + eraseFull) return 0; if (d >= gap + fadeEnd) return 1; return (d - (gap + eraseFull)) / (fadeEnd - eraseFull); }
    for(let i=1;i<buf.length;i++){
      const a = buf[i-1], b = buf[i]; const ta = a.t % win, tb = b.t % win;
      const xa = X(a.t), xb = X(b.t);
      // wrapped: draw a small dot at xb
      if (tb < ta){
        const r = Math.max(1, Math.min(3, Math.round(Math.max(1, Math.min(3, Math.floor(W/200))))));
        const d = distAheadPx(xb);
        const alpha = alphaFromDist(d);
        if (alpha > 0){ ctx.globalAlpha = alpha; ctx.fillStyle = cfg.color; ctx.beginPath(); ctx.arc(xb, Y(b.v), r, 0, Math.PI*2); ctx.fill(); ctx.globalAlpha = 1; }
      } else {
        // compute alpha for endpoints and use that to draw the segment (approximate)
        const da = distAheadPx(xa), db = distAheadPx(xb);
        const aa = alphaFromDist(da), ab = alphaFromDist(db);
        const segAlpha = Math.max(aa, ab);
        if (segAlpha <= 0) continue; // fully erased
        ctx.globalAlpha = segAlpha;
        if (cfg.step){
          // horizontal segment at a.v from xa -> xb
          ctx.beginPath(); ctx.strokeStyle = cfg.color; ctx.moveTo(xa, Y(a.v)); ctx.lineTo(xb, Y(a.v)); ctx.stroke();
          // vertical transition at xb from a.v -> b.v
          ctx.beginPath(); ctx.moveTo(xb, Y(a.v)); ctx.lineTo(xb, Y(b.v)); ctx.stroke();
        } else {
          // default linear interpolation
          ctx.beginPath(); ctx.strokeStyle = cfg.color; ctx.moveTo(xa, Y(a.v)); ctx.lineTo(xb, Y(b.v)); ctx.stroke();
        }
        ctx.globalAlpha = 1;
      }
    }
    // Erase a short region slightly in front of the newest sample so the head has a small gap
    // before erasure. This creates the visual separation you requested.
    try{
      const last = buf[buf.length-1];
      const xb_latest = X(last.t);
      const gap = 8; // px gap in front of newest sample to keep visible
      const eraseFull = 50; // fully erased region after the gap
      const fadeEnd = 100;  // fade to transparent by this distance after the gap
      const totalW = fadeEnd + 2; // small padding to avoid 1px seams

      ctx.save();
      ctx.globalCompositeOperation = 'destination-out';
      // draw erase region starting at xb_latest + gap
      const start = Math.round(xb_latest + gap);
      const s = ((start % W) + W) % W;
      function drawSeg(px, w, segOffset){
        // segOffset: distance from logical start (0..totalW)
        const segLeft = segOffset;
        const segRight = segOffset + w;
        // fully erased portion relative to segment
        const fullyLeft = 0;
        const fullyRight = eraseFull;
        // if nothing to erase in this seg
        if (segRight <= fullyLeft) return;
        if (segLeft >= fadeEnd) return;
        // solid part
        if (segLeft < fullyRight){
          const left = Math.max(segLeft, fullyLeft);
          const right = Math.min(segRight, fullyRight);
          const pxL = Math.round(px + (left - segLeft));
          const pxW = Math.round(right - left);
          if (pxW>0) ctx.fillRect(pxL, 0, pxW, H);
        }
        // gradient part
        const gradLeft = Math.max(segLeft, fullyRight);
        const gradRight = Math.min(segRight, fadeEnd);
        if (gradRight > gradLeft){
          const pxL = Math.round(px + (gradLeft - segLeft));
          const pxR = Math.round(px + (gradRight - segLeft));
          const g = ctx.createLinearGradient(pxL,0,pxR,0);
          g.addColorStop(0, 'rgba(0,0,0,1)');
          g.addColorStop(1, 'rgba(0,0,0,0)');
          ctx.fillStyle = g;
          ctx.fillRect(pxL,0,pxR-pxL,H);
        }
      }
      if (s + totalW <= W){
        drawSeg(s, totalW, 0);
      } else {
        const w1 = W - s; const w2 = totalW - w1;
        drawSeg(s, w1, 0);
        drawSeg(0, w2, w1);
      }
      ctx.restore();
    }catch(e){ /* safe: ignore erase if anything fails */ }
    }
  window.addEventListener('resize', ()=>{ lastW=0; lastH=0; render(); });
  // expose getSmoothed and setAlpha to allow numeric displays and remote control to read/update smoothing
  function getSmoothed(){ return _prevSmoothed; }
  function setAlpha(a){ if (typeof a === 'number'){ _alpha = Math.max(0, Math.min(1, a)); } }
  function prune(){ if(!buf.length) return; const win = getWin(); const now = buf[buf.length-1].t; while(buf.length && (now - buf[0].t) > win) buf.shift(); }
  return { push, render, getSmoothed, setAlpha, prune }; }

// Apply light exponential smoothing to physiological traces so they look less noisy
// but remain responsive. Tunable via smoothAlpha (0..1). Lower = smoother, Higher = more responsive.
const sAtr = makeStrip('cv-atr',{min:-5,max:205,color:'#0ea5e9', smoothAlpha:0.1});
const sVent= makeStrip('cv-vent',{min:-5,max:205,color:'#ef4444', smoothAlpha:0.1});
const sFlow= makeStrip('cv-flow',{min:0,max:7.5,color:'#f59e0b', smoothAlpha:0.25});
const sValve=makeStrip('cv-valve',{min:0,max:1,color:'#22c55e', step:true});
// PWM should display actual hardware values (no client-side smoothing)
const sPwm = makeStrip('cv-pwm',{min:0,max:255,color:'#a78bfa', step:true});
// Initialize window slider (global window.winSec). Keep default in localStorage or 5s.
window.winSec = Number(localStorage.getItem('winSec')) || 5;
// Wire the slider (if present) to update window.winSec and prune existing buffers.
try{
  const _winLabel = $('winLabel'); const _winRange = $('winRange');
  if(_winRange){ _winRange.value = window.winSec; if(_winLabel) _winLabel.textContent = window.winSec; _winRange.addEventListener('input', (e)=>{
    window.winSec = Number(e.target.value)|0; if(_winLabel) _winLabel.textContent = window.winSec; localStorage.setItem('winSec', window.winSec);
    try{ [sAtr,sVent,sFlow,sValve,sPwm].forEach(s=> s && s.prune && s.prune()); }catch(e){}
    requestStripRender();
  }); }
}catch(e){}

// Centralized render request: schedule a single rAF to render all strips. This
// avoids multiple paints per incoming SSE message (we update several strips
// per message). Also use this loop to compute a render-based FPS metric.
let _stripRenderScheduled = false;
let _renderLast = performance.now(); let _renderEma = 0;
function requestStripRender(){ if(!_stripRenderScheduled){ _stripRenderScheduled = true; requestAnimationFrame((ts)=>{ _stripRenderScheduled = false; const now = performance.now(); const dt = now - _renderLast; _renderLast = now; const fps = 1000/Math.max(1, dt); _renderEma = _renderEma ? (_renderEma * 0.9 + fps * 0.1) : fps; if($('fps')) $('fps').textContent = Math.round(_renderEma); sAtr.render(); sVent.render(); sFlow.render(); sValve.render(); sPwm.render(); }); } }

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
if($('pwmIn')){
  $('pwmIn').addEventListener('change', ()=>{ const v=Number($('pwmIn').value||0); post('/api/pwm?duty='+Math.max(0,Math.min(255,v))); });
  // allow Enter to submit while typing
  $('pwmIn').addEventListener('keydown', (e)=>{ if(e.key==='Enter'){ e.preventDefault(); const v=Number($('pwmIn').value||0); post('/api/pwm?duty='+Math.max(0,Math.min(255,v))); $('pwmIn').blur(); } });
}
if($('bpmIn')){
  $('bpmIn').addEventListener('change', ()=>{ const v=Number($('bpmIn').value||30); post('/api/bpm?b='+Math.max(1,Math.min(60,v))); });
  $('bpmIn').addEventListener('keydown', (e)=>{ if(e.key==='Enter'){ e.preventDefault(); const v=Number($('bpmIn').value||30); post('/api/bpm?b='+Math.max(1,Math.min(60,v))); $('bpmIn').blur(); } });
}
document.querySelectorAll('#modeSeg button').forEach(b=> b.addEventListener('click', e=>{ post('/api/mode?m='+b.dataset.m); }));

function adjustPwm(d){ const el=$('pwmIn'); if(!el) return; let v=Number(el.value||0); v = Math.max(0, Math.min(255, v + d)); el.value = v; post('/api/pwm?duty='+v); }
function adjustBpm(d){ const el=$('bpmIn'); if(!el) return; let v=Number(el.value||0); v = Math.max(1, Math.min(60, v + d)); el.value = v; post('/api/bpm?b='+v); }

// SSE receiver — uses server keys: atr_mmHg, vent_mmHg, flow_L_min, pwmSet, pwm, valve, mode, bpm, loopMs
let last=performance.now(), ema=0;
// diagnostics: rolling arrays for arrival/server intervals and skew (ms)
const _sseArr = []; const _sseSrv = []; const _sseSkew = []; let _lastServerTs = null; let _sseOffset = null;
// Map wall-clock epoch (Date.now) to performance.now timeline so server
// timestamps (millis) can be converted to performance.now() seconds safely.
const _perfEpoch = Date.now() - performance.now();
function stats(a){ if(!a||!a.length) return {n:0,mean:0,sd:0,max:0}; let n=a.length; let sum=0, sumsq=0, max=0; for(const v of a){ sum+=v; sumsq+=v*v; if(v>max) max=v; } const mean=sum/n; const variance = Math.max(0, (sumsq - (sum*sum)/n)/n); return {n,mean,sd:Math.sqrt(variance),max}; }
// persistent numeric-display EMAs to avoid jitter and ensure they follow strip smoothing
let _dispAtr = null, _dispVent = null, _dispFlow = null;
// Blood-pressure detection state (client-side): collect max vent per valve segment
const bpState = { lastValve: null, curMaxVent: null, systolic: null, diastolic: null, lastDisplay: null };
es.onopen=()=>$('sse')? $('sse').textContent='OPEN' : null; es.onerror=()=>$('sse')? $('sse').textContent='ERR' : null;
es.onmessage=(ev)=>{ const now=performance.now(); const dt=now-last; last=now; // arrival dt used for diagnostics only; render FPS is measured by rAF
  try{ const d=JSON.parse(ev.data);
    // diagnostics: record client arrival delta and server-reported interval (if present)
    _sseArr.push(dt); if(_sseArr.length>200) _sseArr.shift();
    if (typeof d.tsMs !== 'undefined'){
      const srv = Number(d.tsMs);
      if (_lastServerTs !== null){ _sseSrv.push(srv - _lastServerTs); if(_sseSrv.length>200) _sseSrv.shift(); }
      _lastServerTs = srv;
      if (_sseOffset === null) _sseOffset = Date.now() - srv; // map server millis() -> client epoch
      const skew = Date.now() - (srv + _sseOffset);
      _sseSkew.push(skew); if(_sseSkew.length>200) _sseSkew.shift();
    }
    // update diagnostic pill
    try{ const a=stats(_sseArr), s=stats(_sseSrv), k=stats(_sseSkew); if($('jit')) $('jit').textContent = `${Math.round(a.mean)}ms\u00B1${Math.round(a.sd)} srv${Math.round(s.mean)}ms skew${Math.round(k.mean)}ms`; }catch(e){}
  // push raw scaled values into strips (they will apply configured smoothing)
  const atrRawScaled = Number(d.atr_mmHg) || 0;
  const ventRawScaled = Number(d.vent_mmHg) || 0;
  const flowRawScaled = Number(d.flow_L_min) || 0;
  // prefer server timestamp for X mapping when available to reduce arrival jitter
  let srvPerfSec = null;
  if (typeof d.tsMs !== 'undefined'){
    const srv = Number(d.tsMs);
    // compute server -> client epoch offset if not initialized
    if (_sseOffset === null) _sseOffset = Date.now() - srv; // map server millis -> client epoch ms
    // convert to performance.now() seconds: perf = (srv + offset - perfEpoch)/1000
    srvPerfSec = (srv + _sseOffset - _perfEpoch) / 1000.0;
  }
  sAtr.push(atrRawScaled, srvPerfSec); sVent.push(ventRawScaled, srvPerfSec); sFlow.push(flowRawScaled, srvPerfSec);
  sValve.push(d.valve?1:0, srvPerfSec); sPwm.push(Number(d.pwm)||0, srvPerfSec);
      // If server provided smoothing settings, apply them to the strips so the smoothing is in sync
      try{ if (d.smooth){ if (sAtr.setAlpha) sAtr.setAlpha(Number(d.smooth.atr) || 0); if (sVent.setAlpha) sVent.setAlpha(Number(d.smooth.vent) || 0); if (sFlow.setAlpha) sFlow.setAlpha(Number(d.smooth.flow) || 0); } }catch(e){}
  // For numeric displays, prefer the smoothed value from the strip if available; apply a small EMA here
  // to further reduce jitter and ensure numbers move smoothly with the graphs.
  const alpha_attraw = (d.smooth && typeof d.smooth.atr === 'number') ? Number(d.smooth.atr) : 1.0;
  const alpha_ventraw = (d.smooth && typeof d.smooth.vent === 'number') ? Number(d.smooth.vent) : 1.0;
  const alpha_flowraw = (d.smooth && typeof d.smooth.flow === 'number') ? Number(d.smooth.flow) : 1.0;
  const sAtrVal = (sAtr.getSmoothed && sAtr.getSmoothed() != null) ? sAtr.getSmoothed() : atrRawScaled;
  const sVentVal = (sVent.getSmoothed && sVent.getSmoothed() != null) ? sVent.getSmoothed() : ventRawScaled;
  const sFlowVal = (sFlow.getSmoothed && sFlow.getSmoothed() != null) ? sFlow.getSmoothed() : flowRawScaled;
  // initialize displays on first run
  if (_dispAtr === null) _dispAtr = sAtrVal;
  if (_dispVent === null) _dispVent = sVentVal;
  if (_dispFlow === null) _dispFlow = sFlowVal;
  // apply EMA using the same alpha as the strip (keeps numbers tied to graphs)
  _dispAtr = (_dispAtr * (1 - alpha_attraw)) + (sAtrVal * alpha_attraw);
  _dispVent = (_dispVent * (1 - alpha_ventraw)) + (sVentVal * alpha_ventraw);
  _dispFlow = (_dispFlow * (1 - alpha_flowraw)) + (sFlowVal * alpha_flowraw);
  // Round atrium and ventricle displays to nearest integer (pressure values)
  setNum('n-atr', Math.round(_dispAtr), 0); setNum('n-vent', Math.round(_dispVent), 0); setNum('n-flow', _dispFlow, 2);
  // For PWM numeric display prefer the strip's smoothed value (if available)
  // Display the actual PWM value from the server (do not use client smoothing)
  setNum('n-pwm', Number(d.pwm) || 0, 0);
    if($('n-valve')) $('n-valve').textContent = d.valve? '▲' : '▼';
    // Blood pressure detection (client-side): record max ventricular pressure per valve segment
    if($('n-bp')){
      try{
  const modeN = Number(d.mode);
  const pausedN = Number(d.paused);
  const valveN = Number(d.valve);
  // Use the same smoothed ventricle value that is shown in the UI numeric display
  // (_dispVent is kept in sync with the strip smoothing). Fall back to raw server value
  // if the smoothed display value isn't available.
  const vent = (_dispVent != null) ? _dispVent : (Number(d.vent_mmHg) || 0);
        // Only run when in BEAT mode and unpaused
        if (modeN===2 && pausedN===0){
          if (bpState.lastValve === null){
            // initialize on first beat
            bpState.lastValve = valveN;
            bpState.curMaxVent = vent;
            bpState.systolic = null; bpState.diastolic = null; bpState.lastDisplay = null;
            // remain N/A until first full pair is captured
            $('n-bp').textContent = 'N/A';
          } else {
            if (valveN === bpState.lastValve){
              // same segment: update max
              if (vent > bpState.curMaxVent) bpState.curMaxVent = vent;
            } else {
              // valve changed -> finalize the just-completed segment
              if (bpState.lastValve === 0) bpState.diastolic = bpState.curMaxVent;
              else if (bpState.lastValve === 1) bpState.systolic = bpState.curMaxVent;
              // start new segment
              bpState.lastValve = valveN;
              bpState.curMaxVent = vent;
              // if we have both, update display and reset for next cycle
              if (bpState.systolic != null && bpState.diastolic != null){
                const S = Math.round(bpState.systolic), D = Math.round(bpState.diastolic);
                // Unit ("mmHg") is already shown in the panel HTML; keep the displayed text numeric only.
                bpState.lastDisplay = S + '/' + D;
                $('n-bp').textContent = bpState.lastDisplay;
                bpState.systolic = null; bpState.diastolic = null;
              } else {
                // keep lastDisplay (do not flash N/A between segments)
                if (bpState.lastDisplay) $('n-bp').textContent = bpState.lastDisplay;
              }
            }
          }
        } else {
          // not running/beat -> clear state and show N/A
          bpState.lastValve = null; bpState.curMaxVent = null; bpState.systolic = null; bpState.diastolic = null;
          $('n-bp').textContent = 'N/A';
        }
      }catch(e){ $('n-bp').textContent = 'N/A'; }
    }
      document.querySelectorAll('#modeSeg button').forEach(b=> b.classList.toggle('active', Number(b.dataset.m)===Number(d.mode||0)));
      // Only update input values when the user is not actively typing in them
      const pwmEl = $('pwmIn'); const bpmEl = $('bpmIn');
      if(pwmEl && document.activeElement !== pwmEl) pwmEl.value = d.pwmSet||0;
      if(bpmEl && document.activeElement !== bpmEl) bpmEl.value = d.bpm||0;
      if(d.loopMs && $('loop')) $('loop').textContent = Number(d.loopMs).toFixed(2)+' ms';
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
  "\"cal\":{\"atr_m\":%.6f,\"atr_b\":%.6f,\"vent_m\":%.6f,\"vent_b\":%.6f,\"flow_m\":%.6f,\"flow_b\":%.6f},"
  "\"tsMs\":%lu,"
  "\"smooth\":{\"atr\":%.3f,\"vent\":%.3f,\"flow\":%.3f}}",
        mode, paused, pwmSet, pwm, valve, bpm, loop,
        atr, ven, fl,
        G.atr_raw.load(), G.vent_raw.load(), G.flow_hz.load(),
        G.atr_m.load(), G.atr_b.load(), G.vent_m.load(), G.vent_b.load(), G.flow_m.load(), G.flow_b.load(),
  (unsigned long)millis(),
  (double)g_smooth_atr, (double)g_smooth_vent, (double)g_smooth_flow);
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

  // Live smoothing endpoint - set smoothing alphas for atr/vent/flow (query params 'atr','vent','flow')
  server.on("/api/smooth", HTTP_GET, [](AsyncWebServerRequest* r){
    bool updated = false;
    if (r->hasParam("atr")){
      float v = r->getParam("atr")->value().toFloat(); if (v<0) v=0; if (v>1) v=1; g_smooth_atr = v; updated = true; }
    if (r->hasParam("vent")){
      float v = r->getParam("vent")->value().toFloat(); if (v<0) v=0; if (v>1) v=1; g_smooth_vent = v; updated = true; }
    if (r->hasParam("flow")){
      float v = r->getParam("flow")->value().toFloat(); if (v<0) v=0; if (v>1) v=1; g_smooth_flow = v; updated = true; }
    if (updated) r->send(200, "application/json", "{\"ok\":true}"); else r->send(400);
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
