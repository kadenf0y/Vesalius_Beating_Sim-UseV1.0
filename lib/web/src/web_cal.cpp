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
  #rawLog{height:220px;overflow:auto;border:1px solid #20303a;padding:8px;border-radius:8px;background:#071018;font-family:monospace;font-size:12px}
</style>

<h2>Calibration</h2>

<div class="box">
  <h3>Manual outputs (3 s override)</h3>
  <div class="row">
    <label>PWM (0..255)</label>
    <input id="pwm" type="number" min="0" max="255" value="0">
    <button onclick="setPwm()">Apply</button>
    <button id="btnManualToggle" onclick="toggleManual()">Pause</button>
    <span id="pwmS" class="muted"></span>
  </div>
  <div class="row">
    <label>Valve / Mode</label>
    <button onclick="setMode(0)">Forward (0)</button>
    <button onclick="setMode(1)">Reverse (1)</button>
    <span id="valS" class="muted"></span>
  </div>
</div>

<div class="box">
  <h3>Capture</h3>
  <div class="row">
    <label>Channel</label>
    <div id="chSel">
      <label style="margin-right:8px"><input type="checkbox" data-ch="atr" checked> Atrium</label>
      <label style="margin-right:8px"><input type="checkbox" data-ch="vent" checked> Ventricle</label>
      <label style="margin-right:8px"><input type="checkbox" data-ch="flow"> Flow</label>
    </div>
    <label>Avg N</label><input id="navg" type="number" value="25" min="1">
    <label>Actual mmHg</label><input id="actual_mmHg" type="number" step="0.01" value="0">
    <label>Actual L/min</label><input id="actual_L_min" type="number" step="0.01" value="0">
    <button id="btnCapture" onclick="capture()">Capture</button>
    <button id="btnExportCsv" onclick="exportCsv()">Export CSV</button>
    <span id="capS" class="muted"></span>
  </div>
  <table id="tbl"><thead><tr><th>#</th><th>Channel</th><th>Raw</th><th>Actual</th></tr></thead><tbody></tbody></table>
</div>
<div style="margin-bottom:12px">
  <div id="capProgress" style="height:10px;background:#071018;border:1px solid #20303a;border-radius:6px;overflow:hidden;width:100%"><div id="capBar" style="height:100%;width:0%;background:linear-gradient(90deg,#0ea5e9,#a78bfa)"></div></div>
</div>

<div class="box">
  <h3>Fit / Apply / Persist</h3>
  <div class="row">
    <label>Fit channel</label>
    <select id="fitCh"><option value="atr">Atrium</option><option value="vent">Ventricle</option><option value="flow">Flow</option></select>
    <button onclick="fit()">Fit</button>
    <span id="fitS" class="muted"></span>
  </div>
  <!-- removed per-channel Apply/Save/Load/Defaults buttons (not used) -->
  <div class="box">
    <h3>Calibration coefficients</h3>
    <div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">
      <button id="applyAll">Apply All</button>
      <button id="saveAll">Save All</button>
      <button id="loadAll">Load NVS</button>
      <button id="defaultsAll">Defaults</button>
      <div style="flex:1"></div>
      <span id="calS" class="muted"></span>
    </div>
    <table id="calTable" style="width:100%;border-collapse:collapse">
      <thead><tr><th>Channel</th><th>Current m</th><th>Current b</th><th>Fitted m</th><th>Fitted b</th><th>σ</th><th>R²</th></tr></thead>
      <tbody>
        <tr id="cal-atr" data-ch="atr"><td>Atrium</td><td class="cur-m">-</td><td class="cur-b">-</td><td class="fit-m" contenteditable="true">-</td><td class="fit-b" contenteditable="true">-</td><td class="fit-s">-</td><td class="fit-r2">-</td></tr>
        <tr id="cal-vent" data-ch="vent"><td>Ventricle</td><td class="cur-m">-</td><td class="cur-b">-</td><td class="fit-m" contenteditable="true">-</td><td class="fit-b" contenteditable="true">-</td><td class="fit-s">-</td><td class="fit-r2">-</td></tr>
        <tr id="cal-flow" data-ch="flow"><td>Flow</td><td class="cur-m">-</td><td class="cur-b">-</td><td class="fit-m" contenteditable="true">-</td><td class="fit-b" contenteditable="true">-</td><td class="fit-s">-</td><td class="fit-r2">-</td></tr>
      </tbody>
    </table>
  </div>
</div>

<div class="box">
  <h3>Raw stream</h3>
  <div class="row">
    <button id="clearRaw">Clear</button>
    <div style="flex:1"></div>
    <label class="muted">Auto-scroll</label>
    <input id="autoScroll" type="checkbox" checked>
  </div>
  <div id="rawLog"></div>
</div>

<div class="box">
  <h3>Outgoing commands</h3>
  <div class="row">
    <button id="clearOut">Clear</button>
    <div style="flex:1"></div>
    <label class="muted">Auto-scroll</label>
    <input id="outAuto" type="checkbox" checked>
  </div>
  <div id="outLog" style="height:160px;overflow:auto;border:1px solid #20303a;padding:8px;border-radius:8px;background:#071018;font-family:monospace;font-size:12px"></div>
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
  const body = 'duty=' + encodeURIComponent(String(duty));
  try{
    if (window.logOut) window.logOut('POST /api/pwm_raw  ' + body);
    const r = await fetch('/api/pwm_raw', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: body});
    document.getElementById('pwmS').textContent = r.ok ? 'ok' : 'error';
    if (window.logOut) window.logOut('=> /api/pwm_raw ' + (r.ok? 'OK' : ('ERR ' + r.status)));
  }catch(e){ document.getElementById('pwmS').textContent='fetch err'; if(window.logOut) window.logOut('ERR /api/pwm_raw fetch'); }
}
async function setValve(v){
  // keep raw valve override available if needed
  const dst = document.getElementById('valS');
  try{
    const params = new URLSearchParams(); params.append('dir', String(v));
  if (window.logOut) window.logOut('POST /api/valve_raw  dir=' + String(v));
  const r = await fetch('/api/valve_raw', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: params.toString()});
  if (!r.ok){ const txt = await r.text().catch(()=>'<no body>'); if(dst) dst.textContent = 'error: '+r.status+' '+txt; if(window.logOut) window.logOut('=> /api/valve_raw ERR '+r.status+' '+txt); }
  else { if(dst) dst.textContent = 'ok'; if(window.logOut) window.logOut('=> /api/valve_raw OK'); }
  }catch(e){ if(dst) dst.textContent='fetch err'; console.error(e); }
}

// Use the main-mode API to change forward/reverse/beat like the main UI.
async function setMode(m){
  const dst = document.getElementById('valS');
  try{
    if (window.logOut) window.logOut('GET /api/mode?m=' + String(m));
    const r = await fetch('/api/mode?m='+encodeURIComponent(m));
    if (!r.ok){ const txt = await r.text().catch(()=>'<no body>'); if(dst) dst.textContent = 'error: '+r.status+' '+txt; if(window.logOut) window.logOut('=> /api/mode ERR '+r.status+' '+txt); }
    else { if(dst) dst.textContent = 'ok'; if(window.logOut) window.logOut('=> /api/mode OK'); }
  }catch(e){ if(dst) dst.textContent='fetch err'; console.error(e); }
}

async function toggleManual(){
  // toggle the main play/pause — use same API as the main UI
  await fetch('/api/toggle').catch(()=>{});
  // briefly indicate request sent
  const b = document.getElementById('btnManualToggle'); if(!b) return; b.textContent='...'; setTimeout(()=>b.textContent='Pause', 600);
}

async function capture(){
  // Client-side capture: sample values from the streaming SSE (calEs) so server is not blocked.
  const chEls = Array.from(document.querySelectorAll('#chSel input[data-ch]'));
  const channels = chEls.filter(e=>e.checked).map(e=>e.dataset.ch);
  if (!channels.length){ document.getElementById('capS').textContent='select at least one channel'; return; }
  const n = Math.max(1, Math.min(100000, +document.getElementById('navg').value||25));
  const act_mmHg = +document.getElementById('actual_mmHg').value||0;
  const act_L = +document.getElementById('actual_L_min').value||0;

  const btn = document.getElementById('btnCapture'); if(btn) btn.disabled = true; document.getElementById('capS').textContent='capturing...';
  const bar = document.getElementById('capBar'); if(bar) bar.style.width = '0%';

  // prepare buffers for each channel
  const bufs = {}; channels.forEach(c=> bufs[c]=[]);

  // handler collects samples from calEs messages
  const es = window.calEs;
  if(!es){ document.getElementById('capS').textContent='no stream'; if(btn) btn.disabled=false; return; }

  let collected = 0;
  function onmsg(ev){
    try{
      const d = JSON.parse(ev.data);
      // Push raw telemetry values (not scaled) to buffers so fits operate on raw sensor outputs
      channels.forEach(ch=>{
        if (ch==='atr' && typeof d.atr_raw !== 'undefined') bufs[ch].push(Number(d.atr_raw));
        else if (ch==='vent' && typeof d.vent_raw !== 'undefined') bufs[ch].push(Number(d.vent_raw));
        else if (ch==='flow' && typeof d.flow_hz !== 'undefined') bufs[ch].push(Number(d.flow_hz));
      });
      // use the max length among channels as collected count
      collected = Math.max(...channels.map(c=>bufs[c].length));
      if(bar) bar.style.width = Math.min(100, Math.round((collected / n) * 100)) + '%';
      if(collected >= n){
        // done
        es.removeEventListener('message', onmsg);
        // compute averages and append rows
        channels.forEach(ch=>{
          const arr = bufs[ch];
          if(!arr.length) return;
          const sum = arr.reduce((s,v)=>s+v,0); const avg = sum/arr.length;
          const actual = (ch==='flow')? act_L : act_mmHg;
          addRow(ch, avg, actual);
        });
        document.getElementById('capS').textContent='captured'; if(btn) btn.disabled=false; if(bar) bar.style.width='100%';
      }
    }catch(e){ /* ignore parse errors */ }
  }
  es.addEventListener('message', onmsg);
}

// Export current captured rows[] to a CSV file and trigger download
function exportCsv(){
  try{
    if(!rows || !rows.length){ document.getElementById('capS').textContent='no captured rows'; return; }
    let csv = 'idx,channel,raw,actual\n';
    for(let i=0;i<rows.length;i++){
      const r = rows[i];
      // ensure numeric fields are represented as-is
      csv += `${i+1},${r.ch},${r.raw},${r.actual}\n`;
    }
    const blob = new Blob([csv], {type: 'text/csv;charset=utf-8;'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a'); a.href = url;
    const ts = new Date().toISOString().replace(/[:.]/g,'-');
    a.download = 'cal_capture_' + ts + '.csv';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    if(window.logOut) window.logOut('EXPORT CSV rows=' + rows.length);
    document.getElementById('capS').textContent = 'exported';
  }catch(e){ console.error(e); document.getElementById('capS').textContent='export err'; }
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
  // Use the captured table rows for fitting (client-side)
  const ch = document.getElementById('fitCh').value;
  // gather rows from capture table
  const rows = Array.from(document.querySelectorAll('#tbl tbody tr'));
  const pts = [];
  for (const r of rows){
    const tds = r.querySelectorAll('td');
    if (!tds || tds.length < 4) continue;
    const chName = tds[1].textContent.trim();
    if (chName !== ch) continue;
    const raw = parseFloat(tds[2].textContent);
    const actual = parseFloat(tds[3].textContent);
    if (!isNaN(raw) && !isNaN(actual)) pts.push({x: raw, y: actual});
  }
  if (pts.length < 2){
    document.getElementById('fitS').textContent = 'need ≥2 captured points for fit';
    return;
  }
  const f = linfit(pts);
  // compute standard deviation of residuals (sample stddev)
  let ss = 0;
  for (const p of pts){ const yhat = f.m * p.x + f.b; ss += (p.y - yhat) * (p.y - yhat); }
  const stddev = (pts.length>1) ? Math.sqrt(ss / (pts.length - 1)) : 0;
  document.getElementById('fitS').textContent = `n=${f.n} m=${f.m.toFixed(6)} b=${f.b.toFixed(3)} r²=${f.r2.toFixed(4)} σ=${stddev.toFixed(3)}`;

  // update the calibration table fitted cells for the channel and enable actions
  function setFitted(chKey, m, b, s, r2){
    const row = document.getElementById('cal-' + (chKey==='atr'?'atr':chKey==='vent'?'vent':'flow'));
    if (!row) return;
    row.querySelector('.fit-m').textContent = Number(m).toFixed(6);
    row.querySelector('.fit-b').textContent = Number(b).toFixed(6);
    row.querySelector('.fit-s').textContent = Number(s).toFixed(3);
    row.querySelector('.fit-r2').textContent = Number(r2).toFixed(4);
    // enable apply/save for this channel
    const applyBtn = row.querySelector('.btn-apply'); const saveBtn = row.querySelector('.btn-save');
    // fitted values populated; user may also edit the fitted m/b cells manually (contenteditable)
  }
  setFitted(ch, f.m, f.b, stddev, f.r2);
}


// Refresh current runtime coefficients into the calibration table
async function refreshCalTable(){
  try{
    const r = await fetch('/api/cal/get'); const cur = await r.json();
    // populate current columns
    const mAtr = cur.atr_m || 0, bAtr = cur.atr_b || 0;
    const mVent= cur.vent_m || 0, bVent= cur.vent_b || 0;
    const mFlow= cur.flow_m || 0, bFlow= cur.flow_b || 0;
    const setRow = (id,m,b)=>{
      const row = document.getElementById('cal-'+id); if(!row) return;
      row.querySelector('.cur-m').textContent = Number(m).toFixed(6);
      row.querySelector('.cur-b').textContent = Number(b).toFixed(6);
      // reset fitted cells and disable apply/save
      row.querySelector('.fit-m').textContent = '-'; row.querySelector('.fit-b').textContent = '-'; row.querySelector('.fit-s').textContent = '-'; row.querySelector('.fit-r2').textContent = '-';
      const applyBtn = row.querySelector('.btn-apply'); const saveBtn = row.querySelector('.btn-save'); if(applyBtn) applyBtn.disabled=true; if(saveBtn) saveBtn.disabled=true;
    };
    setRow('atr', mAtr, bAtr); setRow('vent', mVent, bVent); setRow('flow', mFlow, bFlow);
    document.getElementById('calS').textContent = '';
  }catch(e){ document.getElementById('calS').textContent = 'error refreshing'; }
}
refreshCalTable();



// Global helpers
async function applyAll(){
  // copy fitted values (if any) into runtime for each channel, then apply once
  const rcur = await fetch('/api/cal/get'); const cj = await rcur.json();
  ['atr','vent','flow'].forEach(ch=>{
    const row=document.getElementById('cal-'+ch); if(!row) return; const m=parseFloat(row.querySelector('.fit-m').textContent); const b=parseFloat(row.querySelector('.fit-b').textContent);
    if(!isNaN(m) && !isNaN(b)){
      if(ch==='atr'){ cj.atr_m=m; cj.atr_b=b; }
      if(ch==='vent'){ cj.vent_m=m; cj.vent_b=b; }
      if(ch==='flow'){ cj.flow_m=m; cj.flow_b=b; }
    }
  });
  // server expects the payload as a form field named "plain" (see /api/cal/apply handler)
  const payload = 'plain='+encodeURIComponent(JSON.stringify(cj));
  try{
    if (window.logOut) window.logOut('POST /api/cal/apply  plain=' + payload);
    const r = await fetch('/api/cal/apply',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:payload});
    if (window.logOut) window.logOut('=> /api/cal/apply ' + (r.ok? 'OK' : ('ERR '+r.status)));
    document.getElementById('calS').textContent='applied all';
  }catch(e){ if(window.logOut) window.logOut('ERR /api/cal/apply'); document.getElementById('calS').textContent='apply err'; }
  await refreshCalTable();
}

async function saveAll(){ await applyAll(); const r = await fetch('/api/cal/save',{method:'POST'}); document.getElementById('calS').textContent = r.ok? 'saved all':'save error'; }

// Wire up button event listeners after DOM
document.addEventListener('DOMContentLoaded', ()=>{
  document.getElementById('applyAll').addEventListener('click', applyAll);
  document.getElementById('saveAll').addEventListener('click', async ()=>{
    if(window.logOut) window.logOut('ACTION: Save All');
    await saveAll();
  });
  document.getElementById('loadAll').addEventListener('click', async ()=>{ if(window.logOut) window.logOut('POST /api/cal/load'); const r = await fetch('/api/cal/load',{method:'POST'}); if(window.logOut) window.logOut('=> /api/cal/load '+(r.ok?'OK':'ERR '+r.status)); await refreshCalTable(); });
  document.getElementById('defaultsAll').addEventListener('click', async ()=>{ if(window.logOut) window.logOut('POST /api/cal/defaults'); const r = await fetch('/api/cal/defaults',{method:'POST'}); if(window.logOut) window.logOut('=> /api/cal/defaults '+(r.ok?'OK':'ERR '+r.status)); await refreshCalTable(); });
  // per-row action buttons removed — users can edit fitted m/b cells manually, then use Apply All / Save All
});

// Shared SSE for the calibration page: expose calEs and latest parsed telemetry
window.calEs = new EventSource('/stream');
window.calLast = null; // last parsed JSON telemetry (if available)

// Raw SSE stream viewer for calibration page — auto-scroll by default
(function(){
  const log = document.getElementById('rawLog'); const clearBtn = document.getElementById('clearRaw'); const auto = document.getElementById('autoScroll');
  if(!log) return;
  const es = window.calEs;
  es.addEventListener('open', ()=>{});
  es.addEventListener('error', ()=>{});
  es.addEventListener('message', (ev)=>{
    try{
      // append raw text
      const el = document.createElement('div'); el.textContent = new Date().toLocaleTimeString() + '  ' + ev.data;
      el.style.padding='4px'; el.style.borderBottom='1px solid rgba(255,255,255,0.02)'; el.style.wordBreak='break-all';
      log.appendChild(el);
      while(log.children.length>2000) log.removeChild(log.firstChild);
      if(auto && auto.checked) log.scrollTop = log.scrollHeight;

      // try to parse JSON telemetry for use elsewhere (e.g., paused state, client-side capture)
      try{ const d = JSON.parse(ev.data); window.calLast = d; const mb = document.getElementById('btnManualToggle'); if (mb && typeof d.paused !== 'undefined'){ mb.textContent = Number(d.paused)===0 ? 'Pause' : 'Play'; } }catch(e){}
    }catch(e){ }
  });
  if(clearBtn) clearBtn.addEventListener('click', ()=> log.innerHTML='');
})();
// Outgoing command logger
(function(){
  const out = document.getElementById('outLog'); const clearBtn = document.getElementById('clearOut'); const auto = document.getElementById('outAuto');
  if(!out) return;
  window.logOut = function(txt){
    try{
      const el = document.createElement('div'); el.textContent = new Date().toLocaleTimeString() + '  ' + txt; el.style.padding='4px'; el.style.borderBottom='1px solid rgba(255,255,255,0.02)'; out.appendChild(el);
      while(out.children.length>2000) out.removeChild(out.firstChild);
      if(auto && auto.checked) out.scrollTop = out.scrollHeight;
    }catch(e){}
  };
  if(clearBtn) clearBtn.addEventListener('click', ()=> out.innerHTML='');
})();
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
      if (!req->hasParam("ch", true) || !req->hasParam("avgN", true)){
        req->send(400); return;
      }
      String chs = req->getParam("ch", true)->value();
      int n = req->getParam("avgN", true)->value().toInt(); if (n<1) n=1; if (n>10000) n=10000;
      float act_mmHg = 0.0f; float act_L = 0.0f;
      if (req->hasParam("actual_mmHg", true)) act_mmHg = req->getParam("actual_mmHg", true)->value().toFloat();
      if (req->hasParam("actual_L_min", true)) act_L = req->getParam("actual_L_min", true)->value().toFloat();

      auto avgN = [&](std::function<float(void)> fn)->float{
        double acc=0; for (int i=0;i<n;i++){ acc+=fn(); delay(2); } return (float)(acc/n);
      };

      String out = "{\"points\": [";
      bool first = true;
      int start = 0;
      while (start <= chs.length()){
        int comma = chs.indexOf(',', start);
        String ch = (comma == -1) ? chs.substring(start) : chs.substring(start, comma);
        ch.trim();
        if (ch.length()){
          if (!first) out += ",";
          if (ch=="atr" && H.read_atr_raw){
            float raw = avgN([&]{ return (float)H.read_atr_raw(); });
            out += String("{\"ch\":\"atr\",\"raw\":") + raw + ",\"actual\":" + act_mmHg + "}";
          } else if (ch=="vent" && H.read_vent_raw){
            float raw = avgN([&]{ return (float)H.read_vent_raw(); });
            out += String("{\"ch\":\"vent\",\"raw\":") + raw + ",\"actual\":" + act_mmHg + "}";
          } else if (ch=="flow" && H.read_flow_hz){
            float rawHz = avgN([&]{ return H.read_flow_hz(); });
            out += String("{\"ch\":\"flow\",\"raw\":") + rawHz + ",\"actual\":" + act_L + "}";
          } else {
            // unknown channel requested: skip
          }
          first = false;
        }
        if (comma == -1) break; start = comma + 1;
      }
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

  // Accept either a urlencoded form field named "plain" (legacy) or raw application/json body.
  srv.on("/api/cal/apply", HTTP_POST,
    // onRequest (we'll respond from onBody once full body is received)
    [=](AsyncWebServerRequest* req){
      // if the client sent a urlencoded form field, handle it immediately
      if (req->hasParam("plain", true)){
        String body = req->getParam("plain", true)->value();
        // naive parse (small JSON): look for keys
        auto findVal=[&](const char* key)->float{
          int i = body.indexOf(String("\"") + key + "\":"); if (i<0) return NAN;
          i += (String("\"") + key + "\":").length();
          return body.substring(i).toFloat();
        };
        float am=findVal("atr_m"),ab=findVal("atr_b"),vm=findVal("vent_m"),vb=findVal("vent_b"),fm=findVal("flow_m"),fb=findVal("flow_b");
        if (isnan(am)||isnan(ab)||isnan(vm)||isnan(vb)||isnan(fm)||isnan(fb)){ req->send(400); return; }
        H.set_cals(am,ab,vm,vb,fm,fb);
        req->send(200, "application/json", "{\"ok\":true}");
      }
      // otherwise we'll wait for onBody to collect the raw body
    },
    // onUpload (unused)
    nullptr,
    // onBody
    [=](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
      static String body;
      if (index == 0) body = String();
      for (size_t i=0;i<len;i++) body += (char)data[i];
      // only act when full body received
      if (index + len < total) return;
      // content may be JSON; attempt naive parse same as above
      auto findVal=[&](const char* key)->float{
        int i = body.indexOf(String("\"") + key + "\":"); if (i<0) return NAN;
        i += (String("\"") + key + "\":").length();
        return body.substring(i).toFloat();
      };
      float am=findVal("atr_m"),ab=findVal("atr_b"),vm=findVal("vent_m"),vb=findVal("vent_b"),fm=findVal("flow_m"),fb=findVal("flow_b");
      if (isnan(am)||isnan(ab)||isnan(vm)||isnan(vb)||isnan(fm)||isnan(fb)){ req->send(400); return; }
      H.set_cals(am,ab,vm,vb,fm,fb);
      req->send(200, "application/json", "{\"ok\":true}");
    }
  );

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
