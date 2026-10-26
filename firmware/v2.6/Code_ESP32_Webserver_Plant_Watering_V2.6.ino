/**********************************************************************************
 *  AGRIFLOW — Smart Plant Watering System — ESP32 Web Server — V2
 *  (Rebuild of the V1 sketch by Tech StudyCell — same hardware, same wiring,
 *   same calibration. V2 fixes the control bugs and replaces the web page.)
 *
 *  WHAT V2 CHANGES (see CHANGES.md for the full list, in simple words):
 *   1. ONE source of truth for the pump and the mode (pumpOn / autoMode).
 *      Every path — auto logic, web buttons, physical buttons — goes through
 *      setPump()/doSetMode(), so the relay, the OLED, the web page and the
 *      LEDs can never disagree with each other.
 *   2. A real JSON API: GET /data returns all live values. GET /pump and
 *      GET /mode apply a command and return the same JSON, so the web page
 *      always shows the state the device CONFIRMED — never a guess.
 *      A pump command in AUTO mode changes nothing and answers with
 *      "err":"auto-controls-pump" (V1 silently beeped and did nothing).
 *   3. Physical buttons call internal functions only. V1 called the HTTP
 *      handlers from the button handlers, which called server.send() with
 *      no request in progress — that was a bug.
 *   4. No blocking delay() beeps anywhere. The buzzer is a non-blocking
 *      service in loop(), so the web server never freezes for half a second.
 *   5. If the DHT11 read fails, the last good temperature/humidity are kept
 *      and dhtOk=false is reported (V1 displayed a fake 0 C / 0 %).
 *   6. New mobile-first dashboard (single inline page, no internet needed):
 *      live moisture gauge, history sparkline, temperature/humidity cards,
 *      a weather card (loaded by the PHONE from Open-Meteo — see below),
 *      and big touch controls that show the real pump/mode state.
 *
 *  HOW TO USE:
 *   1. Upload this sketch from the Arduino IDE (board: ESP32 Dev Module).
 *      Libraries needed (same as V1): Adafruit SSD1306, Adafruit Unified
 *      Sensor, DHT sensor library by Adafruit, AceButton by bxparks.
 *   2. On your phone, join the WiFi network  "ESP32_Plant_System"
 *      (password: 12345678), then open http://192.168.4.1 in the browser.
 *   3. Physical controls still work exactly as before: button on GPIO32
 *      toggles the pump (Manual mode only), button on GPIO33 switches
 *      Auto/Manual, Mode LED (GPIO15) is ON in Auto mode.
 *
 *  ABOUT THE WEATHER CARD:
 *   The ESP32 runs as its own Access Point, so the BOARD has no internet.
 *   The weather on the dashboard is fetched by the PHONE's browser from the
 *   free Open-Meteo API (no key needed) whenever the phone has a connection.
 *   On the ESP32's own network the phone is usually offline, so the card
 *   quietly shows "unavailable" — watering is completely unaffected.
 *
 *  OPTIONAL — GIVE THE BOARD INTERNET LATER (STA mode):
 *   If you ever want the ESP32 itself online (for example to fetch weather
 *   on the board, or to reach the dashboard from your home WiFi), replace
 *   the WiFi.softAP(...) lines in setup() with the block below and keep
 *   softAP if you also want the Access Point to stay available:
 *
 *       WiFi.mode(WIFI_AP_STA);
 *       WiFi.begin("YourHomeWiFiName", "YourHomeWiFiPassword");
 *       // wait for WiFi.status() == WL_CONNECTED, then print WiFi.localIP()
 *       WiFi.softAP(ssid, password);   // keep the AP as well (optional)
 *
 *   Nothing else in this sketch needs to change for that.
 **********************************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <AceButton.h>
using namespace ace_button;

#define SCREEN_WIDTH 128  // OLED display width, in pixels
#define SCREEN_HEIGHT 32  // OLED display height, in pixels
#define OLED_RESET -1     // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---- Pin map (IDENTICAL to V1 / the project report — do not change) ----
#define SensorPin 34       // capacitive soil moisture sensor (analog in)
#define DHTPin 14          // DHT11 data
#define RelayPin 25        // pump relay / TIP122 driver
#define wifiLed 2          // WiFi status LED
#define RelayButtonPin 32  // physical button: pump
#define ModeSwitchPin 33   // physical button: Auto/Manual
#define BuzzerPin 26       // buzzer
#define ModeLed 15         // ON = Auto mode

#define DHTTYPE DHT11
DHT dht(DHTPin, DHTTYPE);

ButtonConfig config1;
AceButton button1(&config1);
ButtonConfig config2;
AceButton button2(&config2);

const char *ssid = "ESP32_Plant_System";  // AP name
const char *password = "12345678";        // AP password

WebServer server(80);

// Soil sensor calibration (measured with the calibration sketch — as V1)
int wetSoilVal = 1050;  // sensor value in wet soil
int drySoilVal = 3050;  // sensor value in dry soil

// AUTO pump thresholds in percent (V2.5 — lowered on Tim's report).
// Why: OFF at 80% needed a raw reading under 1450, but this probe dipped in
// open water reads higher than the wet-soil calibration point, so the %
// never crossed 80 and the motor kept running with the sensor in water.
// OFF at 50% needs raw under ~2050 — a water dip crosses it easily, and
// genuinely wet soil crosses it too. Tune these two numbers if needed.
int moistPerLow = 30;   // pump ON below this
int moistPerHigh = 50;  // pump OFF above this

// ---- THE state. One source of truth — everything reads/writes via these ----
bool pumpOn = false;    // is the pump actually running?
bool autoMode = true;   // true = AUTO, false = MANUAL

int sensorVal = 0;          // last raw analog reading
int moisturePercentage = 0; // last computed moisture %
float temperatureC = 0;     // last GOOD temperature reading
float humidityPct = 0;      // last GOOD humidity reading
bool dhtOk = false;         // did the last DHT read succeed?

unsigned long previousMillis = 0;
const long interval = 2000;  // sensor + OLED refresh interval (ms)
unsigned long lastAlertMillis = 0;
const long buzzerInterval = 3000;  // manual-mode alert repeat interval (ms)

// Non-blocking buzzer state
bool buzzerOn = false;
unsigned long buzzerOffAt = 0;

// ---- Pump failsafe + stats (V2.4) ----
const unsigned long PUMP_MAX_RUN_MS = 120000;   // hard stop: 2 min continuous max
const unsigned long PUMP_COOLDOWN_MS = 60000;   // then 60 s forced rest
unsigned long pumpStartedAt = 0;      // millis() when the pump last turned ON
unsigned long pumpCooldownUntil = 0;  // failsafe rest runs until this millis()
unsigned long timedWaterUntil = 0;    // timed watering ends at this millis() (0 = none)
unsigned long pumpTotalMs = 0;        // total pump run time since boot
unsigned long lastRunSec = 0;         // duration of the last finished run (s)
int pumpRuns = 0;                     // pump starts since boot

// Moisture-sensor fault: raw pinned at an ADC extreme (0 / 4095) = wire out/short
bool sensorFault = false;
int faultStreak = 0;

// ---------------------------------------------------------------------------
// Dashboard page (single inline HTML file — no external resources, so it
// works on the ESP32's own Access Point where there is no internet).
// Written by build.py from page_template.html — do not edit by hand here.
// ---------------------------------------------------------------------------
const char PAGE_HTML[] PROGMEM = R"AGRIFLOW(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<meta name="theme-color" media="(prefers-color-scheme: dark)" content="#0B100E">
<meta name="theme-color" media="(prefers-color-scheme: light)" content="#F2F5F1">
<link rel="icon" href="data:,">
<title>AGRIFLOW — Smart Plant Watering</title>
<style>
:root{
  --bg:#0B100E; --card:#151C19; --border:#26332C; --text:#EDF5F0; --muted:#A7B8AE;
  --accent:#34D399; --track:#223129; --segBg:#0E1512; --shadow:rgba(0,0,0,.28);
  --pillBg:#0E3B24; --pillTx:#34D399; --pillManBg:#3A2A10; --pillManTx:#FBBF24;
  --badTx:#F87171; --pumpOnBg:#0E3B24;
}
@media (prefers-color-scheme: light){
  :root{
    --bg:#F2F5F1; --card:#FFFFFF; --border:#D8E2D9; --text:#12211A; --muted:#4A5D52;
    --accent:#16A34A; --track:#DFE9E0; --segBg:#E7EEE8; --shadow:rgba(18,33,26,.09);
    --pillBg:#DCF5E5; --pillTx:#16A34A; --pillManBg:#FEF3C7; --pillManTx:#B45309;
    --badTx:#DC2626; --pumpOnBg:#DCF5E5;
  }
}
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
html{overscroll-behavior-y:none}
body{background:var(--bg);color:var(--text);font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;font-size:15px;-webkit-text-size-adjust:100%}
button{font-family:inherit;touch-action:manipulation;cursor:pointer}
header.topbar{position:sticky;top:0;z-index:10;background:var(--bg);border-bottom:1px solid var(--border);display:flex;justify-content:space-between;align-items:center;padding:calc(8px + env(safe-area-inset-top)) 16px 8px;min-height:48px}
.brand{font-size:17px;font-weight:800;letter-spacing:2.5px}
.topright{display:flex;align-items:center;gap:9px}
.pill{padding:4px 11px;border-radius:999px;font-size:11px;font-weight:700;letter-spacing:.09em;background:var(--pillBg);color:var(--pillTx);border:1px solid var(--pillTx)}
.pill.off{background:transparent;color:var(--badTx);border-color:var(--badTx)}
.upd{font-size:12px;color:var(--muted);font-variant-numeric:tabular-nums}
.wrap{max-width:480px;margin:0 auto;padding:16px 16px calc(20px + env(safe-area-inset-bottom))}
.card{background:var(--card);border:1px solid var(--border);border-radius:16px;box-shadow:0 1px 3px var(--shadow);padding:16px;margin-bottom:12px}
.label{font-size:11px;font-weight:600;letter-spacing:.09em;text-transform:uppercase;color:var(--muted);margin-bottom:10px}
.hero{padding:20px 16px 16px}
.gaugeBox{position:relative;max-width:340px;margin:0 auto}
svg.gauge{width:100%;height:auto;display:block}
#gaugeArc{transition:stroke-dashoffset .6s ease,stroke .4s ease}
.heroNum{position:absolute;left:0;right:0;top:33%;text-align:center;font-size:60px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums}
.heroNum .sign{font-size:26px;font-weight:700;color:var(--muted)}
.heroStatus{position:absolute;left:0;right:0;top:69%;text-align:center;font-size:13px;font-weight:700;letter-spacing:2px}
.rawline{text-align:center;color:var(--muted);font-size:13px;margin-top:8px;font-variant-numeric:tabular-nums}
.rawline b{color:var(--text);font-weight:600}
.controls{position:sticky;bottom:12px;z-index:9}
.seg button.pending{opacity:.55;pointer-events:none}
.motorHead{display:flex;align-items:center;justify-content:space-between;margin:2px 0 10px}
.motorState{display:inline-flex;align-items:center;gap:7px;font-size:12.5px;font-weight:700;letter-spacing:.08em;padding:5px 12px;border-radius:999px;border:1px solid var(--border);color:var(--muted);background:var(--segBg)}
.motorState::before{content:"";width:8px;height:8px;border-radius:50%;background:var(--muted)}
.motorState.on{color:var(--accent);border-color:var(--accent);background:var(--pumpOnBg)}
.motorState.on::before{background:var(--accent);box-shadow:0 0 8px var(--accent);animation:pulse 1.2s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.35}}
.motorSeg{margin-top:0}
.motorSeg.locked{opacity:.45}
#motorOn.active{background:var(--accent);color:#FFFFFF}
#motorOff.activeOff{background:var(--track);color:var(--text)}
#controlsCard{transition:box-shadow .3s ease,border-color .3s ease}
#controlsCard.running{border-color:var(--accent);box-shadow:0 0 18px rgba(52,211,153,.18)}
.seg{transition:box-shadow .25s ease}
.seg.flash{box-shadow:0 0 0 2px var(--pillManTx)}
#pumpNote{display:none;color:var(--pillManTx);font-size:13px;text-align:center;margin-top:9px;line-height:1.45}
#restNote{display:none;color:var(--badTx);font-size:13px;text-align:center;margin-top:9px;line-height:1.45}
#waterBtn{width:100%;min-height:52px;margin-top:12px;border-radius:12px;border:1px solid var(--accent);background:transparent;color:var(--accent);font-size:15.5px;font-weight:600}
#waterBtn.running{background:var(--pumpOnBg)}
#waterBtn.locked{opacity:.45;border-color:var(--border);color:var(--muted)}
#waterBtn.pending{opacity:.55;pointer-events:none}
#faultBanner{display:none;background:#3B1113;border:1px solid var(--badTx);color:#FCA5A5;border-radius:12px;padding:11px 14px;font-size:14px;line-height:1.45;margin-bottom:12px}
.statRow{display:flex;gap:12px;text-align:center}
.statRow>div{flex:1}
.statRow .val{font-size:24px}
.seg{display:flex;background:var(--segBg);border:1px solid var(--border);border-radius:999px;padding:4px;margin-top:12px;gap:4px}
.seg button{flex:1;height:52px;border:0;border-radius:999px;background:transparent;color:var(--muted);font-size:16px;font-weight:600;letter-spacing:1px}
.seg button.active{background:var(--accent);color:#FFFFFF}
#toast{min-height:20px;text-align:center;color:var(--pillManTx);font-size:14px;margin-top:10px}
.row{display:flex;gap:12px}
.row .card{flex:1}
.val{font-size:28px;font-weight:700;font-variant-numeric:tabular-nums}
.val .unit{font-size:15px;color:var(--muted);font-weight:600}
.note{color:var(--pillManTx);font-size:12px;margin-top:4px;min-height:14px;line-height:1.4}
canvas#spark{width:100%;height:72px;display:block}
.wxbig{display:flex;align-items:baseline;gap:12px}
.wxcond{font-size:17px;font-weight:700}
.wxmeta{color:var(--muted);font-size:14px;margin-top:6px;line-height:1.55}
.wxwhen{color:var(--muted);font-size:12px;margin-top:6px}
.wxoff{color:var(--muted);font-size:14.5px;line-height:1.5}
footer{text-align:center;color:var(--muted);font-size:12px;line-height:1.65;font-variant-numeric:tabular-nums}
.offline .dim{opacity:.55}
.controls.off{opacity:.55}
</style>
</head>
<body>
<!-- dashboard: single inline page, no external resources -->
<header class="topbar">
  <div class="brand">AGRIFLOW</div>
  <div class="topright"><span class="pill" id="livePill">LIVE</span><span class="upd" id="updText">connecting…</span></div>
</header>

<main class="wrap" id="mainWrap">
  <section class="card hero dim">
    <div class="label">Soil Moisture</div>
    <div class="gaugeBox">
      <svg class="gauge" viewBox="0 0 200 118" aria-hidden="true">
        <path d="M 16 102 A 84 84 0 0 1 184 102" fill="none" stroke="var(--track)" stroke-width="16" stroke-linecap="round"/>
        <path id="gaugeArc" d="M 16 102 A 84 84 0 0 1 184 102" fill="none" stroke="#4ADE80" stroke-width="16" stroke-linecap="round" stroke-dasharray="263.89" stroke-dashoffset="263.89"/>
        <line id="dryTick" x1="41" y1="60" x2="29" y2="50" stroke="var(--muted)" stroke-width="2.5"/>
      </svg>
      <div class="heroNum"><span id="moistPct">--</span><span class="sign">%</span></div>
      <div class="heroStatus" id="moistStatus">WAITING</div>
    </div>
    <div class="rawline">Sensor raw <b id="rawVal">----</b> · wet 1050 · dry 3050</div>
  </section>

  <div id="faultBanner">Sensor fault — the pump has been stopped for safety. Check the moisture probe wire, then readings will recover on their own.</div>

  <section class="card controls" id="controlsCard">
    <div class="motorHead"><span class="label" style="margin:0">Motor · Pump</span><span id="motorState" class="motorState off">OFF</span></div>
    <div class="seg motorSeg" id="motorSeg">
      <button id="motorOn">ON</button>
      <button id="motorOff" class="activeOff">OFF</button>
    </div>
    <button id="waterBtn">Water now — run the pump for 10 sec</button>
    <div id="restNote"></div>
    <div id="pumpNote">Auto mode is driving the motor — switch to MANUAL to run it yourself.</div>
    <div class="label" style="margin-top:14px">Mode</div>
    <div class="seg" id="modeSeg">
      <button id="segAuto" class="active">AUTO</button>
      <button id="segManual">MANUAL</button>
    </div>
    <div id="toast"></div>
  </section>

  <div class="row">
    <section class="card dim">
      <div class="label">Temperature</div>
      <div class="val" id="tempVal">--</div>
      <div class="note" id="dhtNote"></div>
    </section>
    <section class="card dim">
      <div class="label">Humidity · Air</div>
      <div class="val" id="humVal">--</div>
      <div class="note"></div>
    </section>
  </div>

  <section class="card dim">
    <div class="label">Moisture History — Last 10 Min · This Session</div>
    <canvas id="spark" height="72"></canvas>
  </section>

  <section class="card">
    <div class="label">Pump Record — since the board started</div>
    <div class="statRow">
      <div><div class="val" id="statRuns">0</div><div class="note">times run</div></div>
      <div><div class="val" id="statTotal">0s</div><div class="note">total time</div></div>
      <div><div class="val" id="statLast">–</div><div class="note">last run</div></div>
    </div>
  </section>

  <section class="card">
    <div class="label">Local Weather · Amaravati · Open-Meteo</div>
    <div id="wxBody" hidden>
      <div class="wxbig"><span class="val" id="wxTemp">--</span><span class="wxcond" id="wxCond"></span></div>
      <div class="wxmeta" id="wxMeta"></div>
      <div class="wxwhen" id="wxWhen"></div>
    </div>
    <div class="wxoff" id="wxOff">Loading weather…</div>
  </section>

  <footer>
    ESP32 · AGRIFLOW · <span id="footIp">192.168.4.1</span> · Uptime <span id="uptime">0:00</span><br>
    Pump ON below <span id="footDry">30</span>% moisture · OFF above <span id="footWet">50</span>%
  </footer>
</main>

<script>
"use strict";
var $=function(id){return document.getElementById(id);};
var ARC=263.89;
var samples=[], lastHistAt=0;
var lastState=null, lastOk=0, offline=false, pending=false, inFlight=false;
var dryBelow=30, wetAbove=50, toastTimer=null;

function bandOf(m){
  if(m<20) return ["CRITICAL","#F87171"];
  if(m<40) return ["DRY","#FBBF24"];
  if(m<=70) return ["IDEAL","#4ADE80"];
  if(m<=85) return ["MOIST","#38BDF8"];
  return ["SATURATED","#A78BFA"];
}
function fmtUp(s){ s=Math.floor(s); var h=Math.floor(s/3600), m=Math.floor((s%3600)/60), ss=s%60; function p(n){return (n<10?"0":"")+n;} return h>0 ? h+":"+p(m)+":"+p(ss) : m+":"+p(ss); }
function fmtDur(s){ s=Math.floor(s||0); if(s<60) return s+"s"; var m=Math.floor(s/60), ss=s%60; if(m<60) return m+"m "+ss+"s"; return Math.floor(m/60)+"h "+(m%60)+"m"; }
function toast(msg){ var t=$("toast"); t.textContent=msg||""; if(toastTimer) clearTimeout(toastTimer); if(msg) toastTimer=setTimeout(function(){ t.textContent=""; },3000); }
function timedFetch(url, ms){
  var ctl=window.AbortController ? new AbortController() : null;
  var to=ctl ? setTimeout(function(){ ctl.abort(); }, ms) : null;
  return fetch(url,{cache:"no-store", signal: ctl ? ctl.signal : undefined})
    .then(function(r){ if(to) clearTimeout(to); if(!r.ok) throw new Error("bad"); return r.json(); },
          function(e){ if(to) clearTimeout(to); throw e; });
}
function setOffline(on){
  offline=on;
  var pill=$("livePill");
  pill.textContent= on ? "OFFLINE" : "LIVE";
  pill.className= "pill" + (on ? " off" : "");
  $("mainWrap").classList.toggle("offline", on);
  $("controlsCard").classList.toggle("off", on);
}
function positionTick(){
  var f=dryBelow/100, a=Math.PI*(1-f), cx=100, cy=102;
  var t=$("dryTick");
  t.setAttribute("x1",(cx+68*Math.cos(a)).toFixed(1)); t.setAttribute("y1",(cy-68*Math.sin(a)).toFixed(1));
  t.setAttribute("x2",(cx+90*Math.cos(a)).toFixed(1)); t.setAttribute("y2",(cy-90*Math.sin(a)).toFixed(1));
}
function drawSpark(){
  var cv=$("spark"); if(!cv.clientWidth) return;
  var dpr=Math.min(window.devicePixelRatio||1, 2), w=cv.clientWidth, h=72;
  if(cv.width!==Math.round(w*dpr)){ cv.width=Math.round(w*dpr); cv.height=Math.round(h*dpr); }
  var ctx=cv.getContext("2d"); ctx.setTransform(dpr,0,0,dpr,0,0); ctx.clearRect(0,0,w,h);
  function Y(v){ return h-4-(v/100)*(h-10); }
  ctx.strokeStyle="rgba(128,150,138,.45)"; ctx.setLineDash([4,4]); ctx.lineWidth=1;
  ctx.beginPath(); ctx.moveTo(0,Y(dryBelow)); ctx.lineTo(w,Y(dryBelow)); ctx.stroke(); ctx.setLineDash([]);
  if(samples.length<2) return;
  var step=w/(samples.length-1);
  ctx.beginPath();
  samples.forEach(function(v,i){ var x=i*step, y=Y(v); if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y); });
  ctx.strokeStyle="#4ADE80"; ctx.lineWidth=2; ctx.stroke();
  ctx.lineTo((samples.length-1)*step,h); ctx.lineTo(0,h); ctx.closePath();
  ctx.fillStyle="rgba(74,222,128,.13)"; ctx.fill();
  var lx=(samples.length-1)*step, ly=Y(samples[samples.length-1]);
  ctx.beginPath(); ctx.arc(lx,ly,3.5,0,6.29); ctx.fillStyle="#4ADE80"; ctx.fill();
  ctx.beginPath(); ctx.arc(lx,ly,6.5,0,6.29); ctx.strokeStyle="rgba(74,222,128,.45)"; ctx.lineWidth=1.5; ctx.stroke();
}
function render(d){
  lastState=d;
  if(typeof d.dryBelow==="number"){ dryBelow=d.dryBelow; wetAbove=d.wetAbove; }
  $("footDry").textContent=dryBelow; $("footWet").textContent=wetAbove;
  positionTick();
  $("moistPct").textContent=d.moisture;
  var b=bandOf(d.moisture);
  var sEl=$("moistStatus"); sEl.textContent=b[0]; sEl.style.color=b[1];
  var arc=$("gaugeArc"); arc.style.stroke=b[1]; arc.style.strokeDashoffset=(ARC*(1-d.moisture/100)).toFixed(2);
  $("rawVal").textContent=d.raw;
  $("tempVal").innerHTML= d.dhtOk ? d.temp.toFixed(1)+' <span class="unit">°C</span>' : "--";
  $("humVal").innerHTML= d.dhtOk ? Math.round(d.hum)+' <span class="unit">%</span>' : "--";
  $("dhtNote").textContent= d.dhtOk ? "" : "DHT11 not responding — last good reading shown";
  var ms=$("motorState");
  ms.textContent= d.pump ? "RUNNING" : "OFF";
  ms.className= "motorState" + (d.pump ? " on" : " off");
  if(!pending){
    $("motorOn").className= d.pump ? "active" : "";
    $("motorOff").className= d.pump ? "" : "activeOff";
  }
  $("motorSeg").classList.toggle("locked", d.auto);
  $("controlsCard").classList.toggle("running", d.pump);
  $("pumpNote").style.display= d.auto ? "block" : "none";
  $("faultBanner").style.display= d.fault ? "block" : "none";
  var rn=$("restNote");
  if(d.coolSec>0){ rn.style.display="block"; rn.textContent="Failsafe rest — the motor was stopped after a long run. It can start again in "+d.coolSec+"s."; }
  else rn.style.display="none";
  var wb=$("waterBtn");
  wb.classList.toggle("locked", d.auto);
  if(d.timedSec>0){ wb.classList.add("running"); wb.textContent="Watering… stops in "+d.timedSec+"s"; }
  else { wb.classList.remove("running"); wb.textContent="Water now — run the pump for 10 sec"; }
  $("statRuns").textContent= d.runs;
  $("statTotal").textContent= fmtDur(d.pumpSec);
  $("statLast").textContent= d.lastSec>0 ? fmtDur(d.lastSec) : "–";
  $("segAuto").className= d.auto ? "active" : "";
  $("segManual").className= d.auto ? "" : "active";
  $("uptime").textContent=fmtUp(d.uptimeSec);
  var now=Date.now();
  if(now-lastHistAt>=5000 || samples.length===0){ samples.push(d.moisture); if(samples.length>120) samples.shift(); lastHistAt=now; }
  drawSpark();
}
function fetchData(){
  if(inFlight || document.hidden) return;
  inFlight=true;
  timedFetch("/data",1500)
    .then(function(d){ inFlight=false; lastOk=Date.now(); if(offline) setOffline(false); render(d); })
    .catch(function(){ inFlight=false; });
}
function cmd(path, btn){
  if(offline){ toast("Offline — waiting for the device to answer."); return; }
  if(pending) return;
  pending=true;
  if(btn) btn.classList.add("pending");
  if(navigator.vibrate){ try{ navigator.vibrate(10); }catch(e){} }
  timedFetch(path,3000)
    .then(function(d){ pending=false; if(btn) btn.classList.remove("pending"); lastOk=Date.now(); if(offline) setOffline(false); render(d);
      if(d.err==="auto-controls-pump") toast("Auto mode controls the pump — switch to Manual first.");
      else if(d.err==="failsafe-rest") toast("Failsafe rest is on — the motor can start again shortly."); })
    .catch(function(){ pending=false; if(btn) btn.classList.remove("pending"); if(lastState) render(lastState); toast("No answer from the device — nothing was changed."); });
}
function pulseMode(){ var s=$("modeSeg"); s.classList.add("flash"); setTimeout(function(){ s.classList.remove("flash"); },950); }
$("motorOn").addEventListener("click",function(){
  if(lastState && lastState.auto){ toast("Auto mode is driving the motor — switch to Manual first."); pulseMode(); return; }
  cmd("/pump?state=on", $("motorOn"));
});
$("motorOff").addEventListener("click",function(){
  if(lastState && lastState.auto){ toast("Auto mode is driving the motor — switch to Manual first."); pulseMode(); return; }
  cmd("/pump?state=off", $("motorOff"));
});
$("waterBtn").addEventListener("click",function(){
  if(lastState && lastState.auto){ toast("Auto mode is driving the motor — switch to Manual first."); pulseMode(); return; }
  if(lastState && lastState.coolSec>0){ toast("Failsafe rest is on — the motor can start again shortly."); return; }
  cmd("/water?sec=10", $("waterBtn"));
});
$("segAuto").addEventListener("click",function(){ cmd("/mode?state=auto", $("segAuto")); });
$("segManual").addEventListener("click",function(){ cmd("/mode?state=manual", $("segManual")); });
setInterval(fetchData,2000);
setInterval(function(){
  if(!lastOk){ $("updText").textContent="connecting…"; return; }
  var s=Math.round((Date.now()-lastOk)/1000);
  $("updText").textContent= s<2 ? "updated just now" : "updated "+s+"s ago";
  if(s>6 && !offline) setOffline(true);
},1000);
document.addEventListener("visibilitychange",function(){ if(!document.hidden) fetchData(); });
window.addEventListener("resize",drawSpark);
$("footIp").textContent= location.host || "192.168.4.1";
fetchData();

var WMO={0:"Clear sky",1:"Mainly clear",2:"Partly cloudy",3:"Overcast",45:"Fog",48:"Fog",51:"Light drizzle",53:"Drizzle",55:"Dense drizzle",56:"Freezing drizzle",57:"Freezing drizzle",61:"Light rain",63:"Rain",65:"Heavy rain",66:"Freezing rain",67:"Freezing rain",71:"Light snow",73:"Snow",75:"Heavy snow",77:"Snow grains",80:"Light showers",81:"Rain showers",82:"Violent showers",85:"Snow showers",86:"Snow showers",95:"Thunderstorm",96:"Thunderstorm with hail",99:"Thunderstorm with hail"};
var WX_KEY="agriflow_wx_v1";
function wxRender(c, whenText){
  $("wxTemp").innerHTML=c.temperature_2m.toFixed(1)+' <span class="unit">°C</span>';
  $("wxCond").textContent=WMO[c.weather_code]||"—";
  $("wxMeta").textContent="Feels "+c.apparent_temperature.toFixed(1)+"° · Humidity "+c.relative_humidity_2m+"% · Wind "+c.wind_speed_10m+" km/h · Rain "+c.precipitation+" mm";
  $("wxWhen").textContent=whenText;
  $("wxBody").hidden=false; $("wxOff").hidden=true;
}
function wxFail(){
  try{
    var raw=localStorage.getItem(WX_KEY);
    if(raw){ var o=JSON.parse(raw); if(o && (Date.now()-o.t)<3*3600*1000){
      wxRender(o.c, "Saved weather from "+new Date(o.t).toLocaleTimeString()+" — no live connection right now");
      return;
    }}
  }catch(e){}
  $("wxBody").hidden=true;
  var off=$("wxOff"); off.hidden=false; off.textContent="Weather unavailable — no internet on this connection";
}
function loadWeather(){
  var u="https://api.open-meteo.com/v1/forecast?latitude=16.5062&longitude=80.6480&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,precipitation,is_day&timezone=auto&forecast_days=1";
  var ctl=window.AbortController ? new AbortController() : null;
  var to=ctl ? setTimeout(function(){ ctl.abort(); },4000) : null;
  fetch(u,{signal: ctl ? ctl.signal : undefined})
    .then(function(r){ if(to) clearTimeout(to); if(!r.ok) throw new Error("bad"); return r.json(); })
    .then(function(j){ var c=j.current; if(!c) throw new Error("bad");
      try{ localStorage.setItem(WX_KEY, JSON.stringify({t:Date.now(), c:c})); }catch(e){}
      wxRender(c, "Live · Open-Meteo · updates every 10 min"); })
    .catch(function(){ if(to) clearTimeout(to); wxFail(); });
}
loadWeather();
setInterval(loadWeather,600000);
</script>
</body>
</html>
)AGRIFLOW";

// ---------------------------------------------------------------------------
// Forward declarations (explicit, so the sketch compiles even where the
// Arduino IDE's automatic prototype generation is confused by the large
// web-page string above — this was the V2.0 'beep not declared' error).
// ---------------------------------------------------------------------------
void setPump(bool on);
void doSetMode(bool auto_);
void doToggleMode();
bool doPumpCommand(const String &state);
bool doTogglePump();
void beep(unsigned long durationMs);
void buzzerService();
void alertService();
void getMoisture();
void readDHT();
void controlPumpAuto();
void updateOLED();
String buildJson(const char *err);
void sendJson(const char *err);
void handleRoot();
void handleData();
void handlePump();
void handleMode();
void handleToggle();
void handleWater();
void pumpGuardService();
void button1Handler(AceButton *button, uint8_t eventType, uint8_t buttonState);
void button2Handler(AceButton *button, uint8_t eventType, uint8_t buttonState);
void showOledEvent(const char *label, const char *value);
void drawOledEvent();

// ---------------------------------------------------------------------------
// OLED event flash (V2.3, Tim's request): when the mode changes or the motor
// turns on/off, the OLED shows it full-screen for ~2 seconds, then the
// normal screen returns. Triggered from setPump()/doSetMode(), so it fires
// for web buttons, physical buttons AND auto-mode pump changes alike.
// ---------------------------------------------------------------------------
String oledEventLabel = "";
String oledEventValue = "";
unsigned long oledEventUntil = 0;
bool oledEventActive = false;

void drawOledEvent() {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(46, 1);
    display.print(oledEventLabel);
    display.setTextSize(2);
    display.setCursor(28, 13);
    display.print(oledEventValue);
    display.display();
}
void showOledEvent(const char *label, const char *value) {
    oledEventLabel = label;
    oledEventValue = value;
    oledEventUntil = millis() + 2000;
    oledEventActive = true;
    drawOledEvent();
}

// ---------------------------------------------------------------------------
// Core state control — the ONLY places the pump/mode are allowed to change.
// ---------------------------------------------------------------------------
void setPump(bool on) {
    if (on && !pumpOn && millis() < pumpCooldownUntil) return;  // failsafe rest
    if (on == pumpOn) return;
    pumpOn = on;
    digitalWrite(RelayPin, on ? HIGH : LOW);
    if (on) {
        pumpStartedAt = millis();
        pumpRuns++;
    } else {
        pumpTotalMs += millis() - pumpStartedAt;
        lastRunSec = (millis() - pumpStartedAt) / 1000;
        timedWaterUntil = 0;
    }
    showOledEvent("MOTOR", on ? "ON" : "OFF");
    Serial.println(on ? "Pump: ON" : "Pump: OFF");
}

void doSetMode(bool auto_) {
    if (autoMode == auto_) return;
    autoMode = auto_;
    digitalWrite(ModeLed, autoMode ? HIGH : LOW);
    showOledEvent("MODE", auto_ ? "AUTO" : "MANUAL");
    beep(120);
    Serial.println(autoMode ? "Mode: AUTO" : "Mode: MANUAL");
}

void doToggleMode() { doSetMode(!autoMode); }

// Returns false when AUTO mode refuses the command (caller reports the error).
bool doPumpCommand(const String &state) {
    if (autoMode) return false;
    bool wantOn = (state == "on") || (state != "off" && !pumpOn);
    if (wantOn && millis() < pumpCooldownUntil) return false;  // failsafe rest
    timedWaterUntil = 0;  // any manual command cancels a timed run
    setPump(wantOn);
    beep(120);
    return true;
}

bool doTogglePump() { return doPumpCommand("toggle"); }

// ---------------------------------------------------------------------------
// Buzzer — fully non-blocking. beep() just schedules; buzzerService() in
// loop() switches it off. The web server never waits on a beep (V1 used
// delay(500) inside its handlers and froze the whole server).
// ---------------------------------------------------------------------------
void beep(unsigned long durationMs) {
    digitalWrite(BuzzerPin, HIGH);
    buzzerOn = true;
    buzzerOffAt = millis() + durationMs;
}

void buzzerService() {
    if (buzzerOn && (long)(millis() - buzzerOffAt) >= 0) {
        digitalWrite(BuzzerPin, LOW);
        buzzerOn = false;
    }
}

// V1 behaviour, kept: in MANUAL mode, beep repeatedly while the soil is too
// dry with the pump off, or too wet with the pump on. Now non-blocking.
void alertService() {
    if (autoMode) return;
    bool needsAlert = (moisturePercentage < moistPerLow && !pumpOn) ||
                      (moisturePercentage > moistPerHigh && pumpOn);
    if (needsAlert && (millis() - lastAlertMillis >= (unsigned long)buzzerInterval)) {
        lastAlertMillis = millis();
        beep(250);
    }
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
void getMoisture() {
    sensorVal = analogRead(SensorPin);
    // Sensor-fault detection (V2.4, corrected in V2.6): ONLY a raw reading
    // pinned at 0 is a fault — that means the sensor signal is shorted to
    // ground. The top rail (4095) is NOT a fault: this probe sits at ~4095
    // in open air, which is a genuine bone-dry reading (0%). V2.4 treated
    // both rails as faults, so AUTO showed SENSOR FAULT whenever the probe
    // was out of the soil. 3 samples in a row (6 s) trips the fault, stops
    // the pump and blocks AUTO until readings look sane again.
    if (sensorVal <= 0) {
        if (++faultStreak >= 3 && !sensorFault) {
            sensorFault = true;
            if (pumpOn) setPump(false);
            beep(250);
            Serial.println("SENSOR FAULT: raw pinned at 0 (signal short)");
        }
    } else {
        faultStreak = 0;
        sensorFault = false;
    }
    moisturePercentage = map(sensorVal, drySoilVal, wetSoilVal, 0, 100);
    moisturePercentage = constrain(moisturePercentage, 0, 100);
}

// On failure keep the last good values and flag it — never show a fake 0.
void readDHT() {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (isnan(t) || isnan(h)) {
        dhtOk = false;
        Serial.println("DHT read failed - keeping last good values");
    } else {
        temperatureC = t;
        humidityPct = h;
        dhtOk = true;
    }
}

// AUTO logic (same thresholds as V1), through setPump() so state stays true.
void controlPumpAuto() {
    if (!autoMode || sensorFault) return;
    if (moisturePercentage < moistPerLow && !pumpOn) setPump(true);
    if (moisturePercentage > moistPerHigh && pumpOn) setPump(false);
}

// OLED layout identical to V1.
void updateOLED() {
    if (oledEventActive) return;  // event flash on screen — don't overwrite it
    if (sensorFault) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(22, 2);
        display.print("SENSOR FAULT");
        display.setTextSize(2);
        display.setCursor(10, 13);
        display.print("CHECK PIN");
        display.display();
        return;
    }
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(30, 2);
    display.print(moisturePercentage); display.print(" %");
    display.setTextSize(1);
    display.setCursor(1, 25);
    display.print("T:"); display.print((int)temperatureC); display.print(" C, ");
    display.print("H:"); display.print((int)humidityPct); display.print("%  ");
    display.print(autoMode ? "A:" : "M:");
    display.print(pumpOn ? "1" : "0");
    display.display();
}

// ---------------------------------------------------------------------------
// JSON API — one builder, so every route returns exactly the same shape.
// ---------------------------------------------------------------------------
String buildJson(const char *err) {
    String j = "{";
    j += "\"moisture\":" + String(moisturePercentage);
    j += ",\"raw\":" + String(sensorVal);
    j += ",\"temp\":" + String(temperatureC, 1);
    j += ",\"hum\":" + String(humidityPct, 1);
    j += ",\"pump\":" + String(pumpOn ? "true" : "false");
    j += ",\"auto\":" + String(autoMode ? "true" : "false");
    j += ",\"dhtOk\":" + String(dhtOk ? "true" : "false");
    j += ",\"uptimeSec\":" + String(millis() / 1000);
    j += ",\"dryBelow\":" + String(moistPerLow);
    j += ",\"wetAbove\":" + String(moistPerHigh);
    j += ",\"runs\":" + String(pumpRuns);
    unsigned long totMs = pumpTotalMs + (pumpOn ? millis() - pumpStartedAt : 0);
    j += ",\"pumpSec\":" + String(totMs / 1000);
    j += ",\"lastSec\":" + String(lastRunSec);
    j += ",\"fault\":" + String(sensorFault ? "true" : "false");
    j += ",\"coolSec\":" + String(millis() < pumpCooldownUntil ? (pumpCooldownUntil - millis()) / 1000 : 0);
    j += ",\"timedSec\":" + String(timedWaterUntil && millis() < timedWaterUntil ? (timedWaterUntil - millis()) / 1000 : 0);
    if (err) { j += ",\"err\":\""; j += err; j += "\""; }
    j += "}";
    return j;
}

void sendJson(const char *err) {
    server.send(200, "application/json", buildJson(err));
}

// ---------------------------------------------------------------------------
// HTTP handlers (server.send() happens ONLY inside these functions)
// ---------------------------------------------------------------------------
void handleRoot() {
    server.send_P(200, "text/html", PAGE_HTML);
}

void handleData() {
    sendJson(nullptr);
}

void handlePump() {
    if (doPumpCommand(server.arg("state"))) sendJson(nullptr);
    else if (autoMode) sendJson("auto-controls-pump");
    else sendJson("failsafe-rest");
}

// Timed watering (V2.4): /water?sec=10 — MANUAL mode only, clamped 5–60 s.
// The pump starts now and pumpGuardService() stops it when time is up.
void handleWater() {
    if (autoMode) { sendJson("auto-controls-pump"); return; }
    if (millis() < pumpCooldownUntil) { sendJson("failsafe-rest"); return; }
    int sec = server.arg("sec").toInt();
    if (sec < 5) sec = 5;
    if (sec > 60) sec = 60;
    timedWaterUntil = millis() + (unsigned long)sec * 1000UL;
    setPump(true);
    beep(120);
    sendJson(nullptr);
}

void handleMode() {
    String s = server.arg("state");
    if (s == "auto") doSetMode(true);
    else if (s == "manual") doSetMode(false);
    else doToggleMode();  // no/unknown state = toggle (legacy behaviour)
    sendJson(nullptr);
}

// Legacy V1 routes — kept so any old bookmark/app still works, now on the
// new logic and returning JSON.
void handleToggle() {
    if (doTogglePump()) sendJson(nullptr);
    else sendJson("auto-controls-pump");
}

// ---------------------------------------------------------------------------
// Physical buttons — internal functions only, never server.send() here.
// ---------------------------------------------------------------------------
void button1Handler(AceButton *button, uint8_t eventType, uint8_t buttonState) {
    if (eventType == AceButton::kEventReleased) {
        Serial.println("Button 1: pump toggle pressed");
        if (!doTogglePump()) Serial.println("Ignored: AUTO mode controls the pump");
    }
}

void button2Handler(AceButton *button, uint8_t eventType, uint8_t buttonState) {
    if (eventType == AceButton::kEventReleased) {
        Serial.println("Button 2: mode toggle pressed");
        doToggleMode();
    }
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    pinMode(SensorPin, INPUT);
    pinMode(RelayPin, OUTPUT);
    pinMode(BuzzerPin, OUTPUT);
    pinMode(ModeLed, OUTPUT);
    pinMode(wifiLed, OUTPUT);
    pinMode(RelayButtonPin, INPUT_PULLUP);
    pinMode(ModeSwitchPin, INPUT_PULLUP);
    setPump(false);
    digitalWrite(BuzzerPin, LOW);
    digitalWrite(ModeLed, autoMode ? HIGH : LOW);

    dht.begin();

    WiFi.softAP(ssid, password);
    digitalWrite(wifiLed, HIGH);
    Serial.println("Access Point Started");
    Serial.print("Dashboard: http://");
    Serial.println(WiFi.softAPIP());

    config1.setEventHandler(button1Handler);
    config2.setEventHandler(button2Handler);
    button1.init(RelayButtonPin);
    button2.init(ModeSwitchPin);

    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("SSD1306 allocation failed"));
        for (;;);
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);

    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.on("/pump", handlePump);
    server.on("/mode", handleMode);
    server.on("/water", handleWater);  // timed watering (V2.4)
    server.on("/toggle", handleToggle);  // legacy (V1)
    server.on("/refresh", handleRoot);  // legacy (V1)
    server.begin();
    Serial.println("HTTP server started");

    getMoisture();
    readDHT();
    updateOLED();
    beep(1000);  // startup beep — non-blocking, buzzerService() ends it
}

// Pump failsafe + timed-watering service (V2.4) — runs every loop pass.
// 1) A timed watering ends itself. 2) No run may exceed PUMP_MAX_RUN_MS:
// the pump is stopped and a cooldown starts, whatever mode asked for it.
void pumpGuardService() {
    if (!pumpOn) return;
    unsigned long now = millis();
    if (timedWaterUntil && now >= timedWaterUntil) { setPump(false); return; }
    if (now - pumpStartedAt >= PUMP_MAX_RUN_MS) {
        setPump(false);
        pumpCooldownUntil = now + PUMP_COOLDOWN_MS;
        showOledEvent("FAILSAFE", "STOPPED");
        Serial.println("FAILSAFE: pump stopped after max run — 60 s rest");
    }
}

void loop() {
    server.handleClient();
    buzzerService();
    pumpGuardService();

    // OLED event flash over? Bring the normal screen back right away.
    if (oledEventActive && millis() >= oledEventUntil) {
        oledEventActive = false;
        updateOLED();
    }

    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= (unsigned long)interval) {
        previousMillis = currentMillis;
        getMoisture();
        readDHT();
        updateOLED();
    }

    alertService();      // manual-mode moisture alerts (non-blocking)
    controlPumpAuto();   // AUTO mode pump logic
    button1.check();
    button2.check();
}
