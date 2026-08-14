#pragma once

#include <Arduino.h>

// Self-contained so the panel works without a filesystem or internet access.
static const char WEB_UI[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AquaESP RS-8</title>
<style>
:root{color-scheme:dark;--bg:#071218;--panel:#d7d9d5;--edge:#879095;--ink:#152026;--green:#50ef86;--amber:#ffbd45;--red:#ff6262;--muted:#82939c}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;background:radial-gradient(circle at 50% -10%,#183746 0,#09171e 42%,#050b0f 100%);font-family:Inter,Segoe UI,Arial,sans-serif;color:#eef8fa;display:grid;place-items:center;padding:28px 14px}
.shell{width:min(920px,100%)}
.topbar{display:flex;justify-content:space-between;align-items:end;gap:20px;margin:0 4px 12px}
.brand{font-size:.78rem;letter-spacing:.22em;text-transform:uppercase;color:#93b4c2}.brand strong{display:block;color:#eefcff;font-size:1.35rem;letter-spacing:.05em;margin-top:4px}
.connection{display:flex;align-items:center;gap:8px;font-size:.82rem;color:#a9bec7}.dot{width:9px;height:9px;border-radius:50%;background:var(--red);box-shadow:0 0 10px #ff626288}.dot.online{background:var(--green);box-shadow:0 0 12px #50ef86aa}
.panel{background:linear-gradient(145deg,#e6e8e4,#b9bfbd);border:1px solid #f5f7f4;border-radius:24px;padding:clamp(18px,4vw,38px);box-shadow:0 28px 70px #000a,inset 0 1px 2px #fff,inset 0 -2px 5px #63707888;color:var(--ink);position:relative}
.panel:before{content:"";position:absolute;inset:9px;border:1px solid #8d9698;border-radius:17px;pointer-events:none}
.header{position:relative;display:grid;grid-template-columns:1fr auto;gap:18px;align-items:center;margin-bottom:22px}
.logo{font-size:1.15rem;font-weight:800;letter-spacing:.08em}.logo small{display:block;font-size:.68rem;font-weight:600;letter-spacing:.18em;color:#53636a;margin-top:2px}
.badges{display:flex;gap:7px;flex-wrap:wrap;justify-content:end}.badge{font-size:.68rem;font-weight:800;letter-spacing:.08em;padding:5px 8px;border-radius:99px;background:#9ea7a8;color:#334147}.badge.ok{background:#baf0c9;color:#145427}.badge.warn{background:#ffe4a5;color:#684800}
.lcd{position:relative;background:linear-gradient(#b8d89e,#a9cb8e);border:8px solid #485158;border-radius:9px;box-shadow:inset 0 0 13px #263f2555,0 2px 4px #fff8;margin-bottom:24px;padding:12px 16px;min-height:72px;display:flex;align-items:center;justify-content:center}
.lcd-text{font-family:Consolas,"Courier New",monospace;font-size:clamp(1.1rem,3.8vw,1.7rem);font-weight:700;letter-spacing:.08em;color:#20331d;text-shadow:0 1px #dff4c8;white-space:pre;overflow:hidden;max-width:100%}
.controls{position:relative;display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.key{appearance:none;border:0;background:linear-gradient(#414b50,#252d31);color:#fff;border-radius:11px;min-height:78px;padding:10px;box-shadow:0 4px 0 #12171a,0 6px 10px #27313855;cursor:pointer;display:grid;grid-template-columns:16px 1fr;align-items:center;gap:9px;text-align:left;font-weight:750;letter-spacing:.025em;transition:transform .08s,filter .15s}
.key:hover:not(:disabled){filter:brightness(1.14)}.key:active:not(:disabled),.key.sending{transform:translateY(3px);box-shadow:0 1px 0 #12171a}.key:disabled{cursor:not-allowed;opacity:.5}
.led{width:12px;height:12px;border-radius:50%;background:#11191c;border:1px solid #697579;box-shadow:inset 0 1px 2px #000}.key.on .led{background:var(--green);border-color:#c8ffda;box-shadow:0 0 12px #28e76c,inset 0 0 2px #fff}.key.enabled .led{background:var(--amber);border-color:#fff0c4;box-shadow:0 0 12px #ffae28}.key.flash .led{background:var(--amber);animation:blink .8s steps(1) infinite}.key.unknown .led{background:#6b7477}
@keyframes blink{50%{background:#182024;box-shadow:none}}
.key-label{font-size:.86rem;line-height:1.15}.key-state{display:block;font-size:.62rem;color:#a9b8bd;text-transform:uppercase;margin-top:4px;letter-spacing:.12em}
.nav{position:relative;display:grid;grid-template-columns:1.15fr 1.15fr .8fr .8fr 1.15fr;gap:9px;margin-top:20px;padding-top:20px;border-top:1px solid #939c9d}
.nav button{appearance:none;border:1px solid #747e82;background:linear-gradient(#fafbf9,#c4c9c7);color:#253137;border-radius:8px;min-height:42px;font-weight:800;box-shadow:0 3px 0 #747d80;cursor:pointer}.nav button:active:not(:disabled){transform:translateY(2px);box-shadow:0 1px 0 #747d80}.nav button:disabled{opacity:.45;cursor:not-allowed}
.footer{display:flex;justify-content:space-between;gap:14px;margin:15px 5px 0;color:#7f99a5;font-size:.72rem}.toast{min-height:18px;text-align:right}.toast.error{color:#ff8585}.toast.good{color:#72ec9b}
@media(max-width:680px){body{padding:12px 8px}.topbar{align-items:center}.panel{border-radius:18px}.controls{grid-template-columns:repeat(2,1fr)}.key{min-height:67px}.header{grid-template-columns:1fr}.badges{justify-content:start}.nav{grid-template-columns:repeat(5,1fr)}.nav button{font-size:.68rem;padding:4px}.footer{flex-direction:column}.toast{text-align:left}}
</style>
</head>
<body>
<main class="shell">
  <div class="topbar"><div class="brand">AquaESP<strong>RS-8 TEST PANEL</strong></div><div class="connection"><span id="netDot" class="dot"></span><span id="netText">Connecting...</span></div></div>
  <section class="panel">
    <div class="header"><div class="logo">JANDY <small>AQUALINK RS8 COMBO</small></div><div class="badges"><span id="busBadge" class="badge">BUS --</span><span id="modeBadge" class="badge">LOADING</span><span id="ackBadge" class="badge">ACK --</span></div></div>
    <div class="lcd"><div id="lcd" class="lcd-text">AQUAESP STARTING</div></div>
    <div id="controls" class="controls"></div>
    <div class="nav">
      <button data-code="9" disabled>MENU</button><button data-code="14" disabled>CANCEL</button><button data-code="19" disabled>&#9664;</button><button data-code="24" disabled>&#9654;</button><button data-code="29" disabled>ENTER</button>
    </div>
  </section>
  <div class="footer"><span id="stats">Waiting for panel state</span><span id="toast" class="toast">&nbsp;</span></div>
</main>
<script>
const buttons=[
  ['FILTER PUMP','filter_pump'],['SPA MODE','spa'],['AUX 1','aux1'],['AUX 2','aux2'],
  ['AUX 3','aux3'],['AUX 4','aux4'],['AUX 5','aux5'],['AUX 6','aux6'],
  ['AUX 7','aux7'],['POOL HEAT','pool_heat'],['SPA HEAT','spa_heat'],['SOLAR HEAT','solar_heat']
];
const controls=document.getElementById('controls');
buttons.forEach(([label,name],index)=>{const el=document.createElement('button');el.className='key unknown';el.disabled=true;el.dataset.index=index;el.innerHTML=`<span class="led"></span><span class="key-label">${label}<span class="key-state">unknown</span></span>`;el.addEventListener('click',()=>pressButton(el,index));controls.appendChild(el)});
const allKeys=()=>document.querySelectorAll('.key,.nav button');
let config={sniff_only:true},busy=false,lastRevision=-1;
function toast(message,type=''){const el=document.getElementById('toast');el.textContent=message;el.className='toast '+type;clearTimeout(toast.timer);toast.timer=setTimeout(()=>{el.innerHTML='&nbsp;';el.className='toast'},2800)}
function setEnabled(enabled){allKeys().forEach(el=>el.disabled=!enabled||busy)}
async function send(path,source){if(busy)return;busy=true;source?.classList.add('sending');setEnabled(false);try{const response=await fetch(path,{method:'POST'});const data=await response.json().catch(()=>({error:`HTTP ${response.status}`}));if(!response.ok)throw new Error(data.error||data.result||`HTTP ${response.status}`);toast('Key queued','good')}catch(error){toast(error.message,'error')}finally{busy=false;source?.classList.remove('sending');setEnabled(!config.sniff_only)} }
function pressButton(el,index){send(`/api/button?index=${index}`,el)}
document.querySelectorAll('.nav button').forEach(el=>el.addEventListener('click',()=>send(`/api/key?code=${el.dataset.code}`,el)));
async function loadConfig(){try{const response=await fetch('/api/config',{cache:'no-store'});if(response.ok)config=await response.json()}catch(_){}const badge=document.getElementById('modeBadge');badge.textContent=config.sniff_only?'SNIFF ONLY':'CONTROL';badge.className='badge '+(config.sniff_only?'warn':'ok');setEnabled(!config.sniff_only);if(config.sniff_only)toast('Controls disabled in sniff-only mode','error')}
function render(state){document.getElementById('netDot').classList.add('online');document.getElementById('netText').textContent=state.ip||'WiFi connected';const bus=document.getElementById('busBadge');bus.textContent=state.online?'BUS ONLINE':'BUS OFFLINE';bus.className='badge '+(state.online?'ok':'warn');const ack=document.getElementById('ackBadge');ack.textContent=`ACK ${state.ack_latency_us||0} us`;ack.className='badge '+((state.ack_latency_us||0)<20000?'ok':'warn');document.getElementById('lcd').textContent=(state.display||'AQUALINK RS8').padEnd(16,' ').slice(0,16);document.getElementById('stats').textContent=`Packets ${state.packets}  |  Queue ${state.keys_queued}  |  RSSI ${state.wifi_rssi??'--'} dBm`;if(Array.isArray(state.buttons)){state.buttons.forEach((item,index)=>{const el=controls.children[index];if(!el)return;el.classList.remove('off','on','flash','enabled','unknown');el.classList.add(item.state||'unknown');el.querySelector('.key-state').textContent=item.state||'unknown'})}lastRevision=state.button_revision;setEnabled(!config.sniff_only&&state.online)}
async function refresh(){try{const response=await fetch('/api/state',{cache:'no-store'});if(!response.ok)throw new Error();render(await response.json())}catch(_){document.getElementById('netDot').classList.remove('online');document.getElementById('netText').textContent='Device unavailable';setEnabled(false)}}
loadConfig().then(refresh);setInterval(refresh,750);
</script>
</body>
</html>
)HTML";
