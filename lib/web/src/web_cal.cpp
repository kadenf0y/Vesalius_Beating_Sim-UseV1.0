#include "web_cal.h"
#include "shared.h"

// Minimal in-page JS/HTML (dark theme). Main affordances: manual raw control,
// capture averages, fit (client-side), apply/save/load/defaults via API.
static const char CAL_HTML[] PROGMEM = R"HTML(
<!doctype html><meta charset="utf-8">
<title>Calibration</title>
<style>
  body{background:#0b0f13;color:#e6edf3;font:14px system-ui;margin:0;padding:12px}
  input,button,select{background:#12181f;color:#e6edf3;border:1px solid #283241;border-radius:8px;padding:6px 10px}
  .row{display:flex;gap:10px;align-items:center;margin:8px 0}
  .box{border:1px solid #283241;border-radius:12px;padding:12px;margin-bottom:12px;background:#0e141b}
  table{width:100%;border-collapse:collapse} th,td{border:1px solid #283241;padding:6px;text-align:left}
  .muted{color:#9aa7b2}
</style>

<h2>Calibration</h2>

<div class="box">
  <h3>Manual outputs (3 s override)</h3>
  <div class="row">
    <label>PWM (0..255)</label>
    <input id="pwm" type="number" min="0" max="255" value="0">
    <button onclick="setPwm()">Apply</button>
    <span id="pwmS" class="muted"></span>
  </div>
  <div class="row">
    <label>Valve</label>
    <button onclick="setValve(0)">Forward (0)</button>
    <button onclick="setValve(1)">Reverse (1)</button>
    <span id="valS" class="muted"></span>
  </div>
</div>

<div class="box">
  <h3>Capture</h3>
  <div class="row">
    <label>Channel</label>
    <select id="ch"><option value="atr">Atrium</option><option value="vent">Ventricle</option><option value="flow">Flow</option></select>
    <label>Avg N</label><input id="navg" type="number" value="25" min="1" max="500">
    <label>Actual</label><input id="actual" type="number" step="0.01" value="0">
    <button onclick="capture()">Capture</button>
    <span id="capS" class="muted"></span>
  </div>
  <table id="tbl"><thead><tr><th>#</th><th>Channel</th><th>Raw</th><th>Actual</th></tr></thead><tbody></tbody></table>
</div>

<div class="box">
  <h3>Fit / Apply / Persist</h3>
  <div class="row">
    <label>Fit channel</label>
    <select id="fitCh"><option value="atr">Atrium</option><option value="vent">Ventricle</option><option value="flow">Flow</option></select>
    <button onclick="fit()">Fit</button>
    <span id="fitS" class="muted"></span>
  </div>
  <div class="row">
    <button onclick="apply()">Apply (runtime)</button>
    <button onclick="save()">Save (NVS)</button>
    <button onclick="load()">Load (NVS→runtime)</button>
    <button onclick="defaults()">Defaults (runtime)</button>
  </div>
  <pre id="cur" class="muted"></pre>
</div>

<script>
let rows=[];
function addRow(ch, raw, actual){
  rows.push({ch,raw,actual});
  const tb=document.querySelector('#tbl tbody'); const tr=document.createElement('tr');
  tr.innerHTML=`<td>${rows.length}</td><td>${ch}</td><td>${raw.toFixed(3)}</td><td>${actual.toFixed(3)}</td>`;
  tb.appendChild(tr);
}

async function setPwm(){
  const duty = Math.max(0, Math.min(255, +document.getElementById('pwm').value||0));
  const r = await fetch('/api/pwm_raw', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'duty='+duty});
  document.getElementById('pwmS').textContent = r.ok ? 'ok' : 'error';
}
async function setValve(v){
  const r = await fetch('/api/valve_raw', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'dir='+v});
  document.getElementById('valS').textContent = r.ok ? 'ok' : 'error';
}

async function capture(){
  const ch=document.getElementById('ch').value;
  const n = Math.max(1, Math.min(1000, +document.getElementById('navg').value||25));
  const act = +document.getElementById('actual').value||0;
  const form = new URLSearchParams({ch, avgN:n, actual:act});
  const r = await fetch('/api/cal/capture', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:form});
  if(!r.ok){ document.getElementById('capS').textContent='capture error'; return; }
  const js = await r.json();
  js.points.forEach(p=> addRow(ch, p.raw, p.actual));
  document.getElementById('capS').textContent='captured';
}

function linfit(v){ // least squares on [{x, y}]
  if(v.length<2) return {m:1,b:0,r2:0,n:v.length};
  let sx=0,sy=0,sxx=0,sxy=0,syy=0;
  for(const p of v){ sx+=p.x; sy+=p.y; sxx+=p.x*p.x; sxy+=p.x*p.y; syy+=p.y*p.y; }
  const n=v.length, denom = n*sxx - sx*sx; if(!denom) return {m:1,b:0,r2:0,n};
  const m = (n*sxy - sx*sy)/denom, b = (sy - m*sx)/n;
  let ssRes=0, ssTot=0, meanY=sy/n;
  for (const p of v){ const yhat=m*p.x+b; ssRes+=(p.y-yhat)**2; ssTot+=(p.y-meanY)**2; }
  return {m,b,r2: ssTot? (1-ssRes/ssTot):0, n};
}

async function fit(){
  const ch=document.getElementById('fitCh').value;
  // collect from server for exactness
  const r = await fetch('/api/cal/list');
  const js = await r.json(); const key = ch;
  const pts = (js[key]||[]).map(p=>({x:p.raw, y:p.actual}));
  const f = linfit(pts);
  document.getElementById('fitS').textContent = `n=${f.n} m=${f.m.toFixed(6)} b=${f.b.toFixed(3)} r²=${f.r2.toFixed(4)}`;
  // apply to runtime cache
  const cur = await fetch('/api/cal/get'); const cj = await cur.json();
  if (ch==='atr')  { cj.atr_m=f.m; cj.atr_b=f.b; }
  if (ch==='vent') { cj.vent_m=f.m; cj.vent_b=f.b; }
  if (ch==='flow') { cj.flow_m=f.m; cj.flow_b=f.b; }
  await fetch('/api/cal/apply',{method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(cj)});
  updateCur();
}
async function apply(){ const cur=await fetch('/api/cal/get'); const cj=await cur.json(); await fetch('/api/cal/apply',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cj)}); updateCur(); }
async function save(){ await fetch('/api/cal/save',{method:'POST'}); updateCur(); }
async function load(){ await fetch('/api/cal/load',{method:'POST'}); updateCur(); }
async function defaults(){ await fetch('/api/cal/defaults',{method:'POST'}); updateCur(); }
async function updateCur(){ const r=await fetch('/api/cal/get'); const j = await r.json(); document.getElementById('cur').textContent = JSON.stringify(j,null,2); }
updateCur();
</script>
)HTML";

void web_cal_register(AsyncWebServer& srv, const CalHooks& H){
  // UI page
  srv.on("/cal", HTTP_GET, [](AsyncWebServerRequest* r){ r->send_P(200, "text/html", CAL_HTML); });

  // Raw outputs (POST) — set override gate (3 s)
  srv.on("/api/pwm_raw", HTTP_POST, [=](AsyncWebServerRequest* req){
    if (!H.write_pwm_raw){ req->send(500); return; }
    if (!req->hasParam("duty", true)){ req->send(400); return; }
    int d = req->getParam("duty", true)->value().toInt();
    if (d<0) d=0; if (d>255) d=255;
    H.write_pwm_raw((uint8_t)d);
    req->send(200, "application/json", "{\"ok\":true}");
  });
  srv.on("/api/valve_raw", HTTP_POST, [=](AsyncWebServerRequest* req){
    if (!H.write_valve_raw){ req->send(500); return; }
    int dir = 0;
    if (req->hasParam("dir", true)) dir = req->getParam("dir", true)->value().toInt();
    dir = dir?1:0;
    H.write_valve_raw((uint8_t)dir);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Capture N raw samples → return averaged point(s)
  srv.on("/api/cal/capture", HTTP_POST, [=](AsyncWebServerRequest* req){
    if (!req->hasParam("ch", true) || !req->hasParam("avgN", true) || !req->hasParam("actual", true)){
      req->send(400); return;
    }
    String ch = req->getParam("ch", true)->value();
    int n = req->getParam("avgN", true)->value().toInt(); if (n<1) n=1; if (n>1000) n=1000;
    float act = req->getParam("actual", true)->value().toFloat();

    auto avgN = [&](std::function<float(void)> fn)->float{
      double acc=0; for (int i=0;i<n;i++){ acc+=fn(); delay(2); } return (float)(acc/n);
    };

    String out = "{\"points\":[";
    if (ch=="atr" && H.read_atr_raw){
      float raw = avgN([&]{ return (float)H.read_atr_raw(); });
      out += String("{\"raw\":")+raw+",\"actual\":"+act+"}";
    } else if (ch=="vent" && H.read_vent_raw){
      float raw = avgN([&]{ return (float)H.read_vent_raw(); });
      out += String("{\"raw\":")+raw+",\"actual\":"+act+"}";
    } else if (ch=="flow" && H.read_flow_hz){
      float rawHz = avgN([&]{ return H.read_flow_hz(); });
      out += String("{\"raw\":")+rawHz+",\"actual\":"+act+"}";
    } else { req->send(400); return; }
    out += "]}";
    req->send(200, "application/json", out);
  });

  // Calibration state get/apply/save/load/defaults
  srv.on("/api/cal/get", HTTP_GET, [=](AsyncWebServerRequest* req){
    float am,ab,vm,vb,fm,fb; H.get_cals(am,ab,vm,vb,fm,fb);
    char buf[256];
    snprintf(buf, sizeof(buf),
      "{\"atr_m\":%.6f,\"atr_b\":%.6f,\"vent_m\":%.6f,\"vent_b\":%.6f,\"flow_m\":%.6f,\"flow_b\":%.6f}",
      am,ab,vm,vb,fm,fb);
    req->send(200, "application/json", buf);
  });

  srv.on("/api/cal/apply", HTTP_POST, [=](AsyncWebServerRequest* req){
    if (!req->hasParam("plain", true)){ req->send(400); return; }
    String body = req->getParam("plain", true)->value();
    // naive parse (small JSON): look for keys
    auto findVal=[&](const char* key)->float{
      int i = body.indexOf(String("\"")+key+"\":"); if (i<0) return NAN;
      i += String("\"")+key+"\":".length();
      return body.substring(i).toFloat();
    };
    float am=findVal("atr_m"),ab=findVal("atr_b"),vm=findVal("vent_m"),vb=findVal("vent_b"),fm=findVal("flow_m"),fb=findVal("flow_b");
    if (isnan(am)||isnan(ab)||isnan(vm)||isnan(vb)||isnan(fm)||isnan(fb)){ req->send(400); return; }
    H.set_cals(am,ab,vm,vb,fm,fb);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  srv.on("/api/cal/save", HTTP_POST, [=](AsyncWebServerRequest* req){
    bool ok = H.nvs_save(true,true,true);
    req->send(ok?200:500, "application/json", ok?"{\"ok\":true}":"{\"ok\":false}");
  });
  srv.on("/api/cal/load", HTTP_POST, [=](AsyncWebServerRequest* req){
    bool ok = H.nvs_load();
    req->send(ok?200:500, "application/json", ok?"{\"ok\":true}":"{\"ok\":false}");
  });
  srv.on("/api/cal/defaults", HTTP_POST, [=](AsyncWebServerRequest* req){
    H.nvs_defaults(); req->send(200, "application/json", "{\"ok\":true}");
  });
}
