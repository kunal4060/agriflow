"use strict";
/* AGRIFLOW cloud dashboard — reads /agriflow/live + /agriflow/history,
 * writes commands to /agriflow/control. One rule throughout: the page
 * only ever displays state the BOARD confirmed (via /live). A control
 * tap sends the command and waits — it never paints a guess. */
var $ = function(id){ return document.getElementById(id); };
var ARC = 263.89;
var samples = [];           // {t, v} moisture history (last 50)
var lastLive = null, lastOk = 0, online = false, pending = false, toastTimer = null;
var dryBelow = 30, wetAbove = 50;
var controlRef = null, liveRef = null, histRef = null, db = null;

function configured(){
  return typeof FIREBASE_CONFIG !== "undefined" &&
    FIREBASE_CONFIG.databaseURL && FIREBASE_CONFIG.databaseURL.indexOf("__") !== 0;
}
function toast(msg){
  var t = $("toast"); t.textContent = msg || "";
  if (toastTimer) clearTimeout(toastTimer);
  if (msg) toastTimer = setTimeout(function(){ t.textContent = ""; }, 4000);
}
function bandOf(m){
  if (m < 20) return ["CRITICAL", "#DC2626"];
  if (m < 40) return ["DRY", "#B45309"];
  if (m <= 70) return ["IDEAL", "#16A34A"];
  if (m <= 85) return ["MOIST", "#0284C7"];
  return ["SATURATED", "#6D28D9"];
}
function fmtAgo(s){
  s = Math.max(0, Math.round(s));
  if (s < 2) return "updated just now";
  if (s < 60) return "updated " + s + "s ago";
  return "updated " + Math.floor(s / 60) + "m ago";
}
function setStatus(mode){
  // mode: "live" | "offline" | "setup"
  var pill = $("livePill");
  if (mode === "live"){ pill.textContent = "LIVE"; pill.className = "pill"; }
  else if (mode === "offline"){ pill.textContent = "OFFLINE"; pill.className = "pill off"; }
  else { pill.textContent = "SETUP"; pill.className = "pill setup"; }
  var dim = (mode !== "live");
  ["heroCard", "statGrid", "chartCard", "controlsCard"].forEach(function(id){
    $(id).classList.toggle("dim", dim);
  });
}
function positionTick(){
  var f = dryBelow / 100, a = Math.PI * (1 - f), cx = 100, cy = 102;
  var t = $("dryTick");
  t.setAttribute("x1", (cx + 68 * Math.cos(a)).toFixed(1));
  t.setAttribute("y1", (cy - 68 * Math.sin(a)).toFixed(1));
  t.setAttribute("x2", (cx + 90 * Math.cos(a)).toFixed(1));
  t.setAttribute("y2", (cy - 90 * Math.sin(a)).toFixed(1));
}
function drawChart(){
  var cv = $("chart"); if (!cv.clientWidth) return;
  var dpr = Math.min(window.devicePixelRatio || 1, 2), w = cv.clientWidth, h = 180;
  if (cv.width !== Math.round(w * dpr)){ cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
  var ctx = cv.getContext("2d"); ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, w, h);
  function Y(v){ return h - 8 - (v / 100) * (h - 18); }
  ctx.strokeStyle = "rgba(74,93,82,.4)"; ctx.setLineDash([4, 4]); ctx.lineWidth = 1;
  [dryBelow, wetAbove].forEach(function(th){
    ctx.beginPath(); ctx.moveTo(0, Y(th)); ctx.lineTo(w, Y(th)); ctx.stroke();
  });
  ctx.setLineDash([]);
  if (samples.length < 2){
    ctx.fillStyle = "#4A5D52"; ctx.font = "14px sans-serif"; ctx.textAlign = "center";
    ctx.fillText("Waiting for history samples…", w / 2, h / 2);
    return;
  }
  var step = w / (samples.length - 1);
  ctx.beginPath();
  samples.forEach(function(p, i){ var x = i * step, y = Y(p.v); if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y); });
  ctx.strokeStyle = "#16A34A"; ctx.lineWidth = 2; ctx.stroke();
  ctx.lineTo((samples.length - 1) * step, h); ctx.lineTo(0, h); ctx.closePath();
  ctx.fillStyle = "rgba(22,163,74,.10)"; ctx.fill();
  var lx = (samples.length - 1) * step, ly = Y(samples[samples.length - 1].v);
  ctx.beginPath(); ctx.arc(lx, ly, 3.5, 0, 6.29); ctx.fillStyle = "#16A34A"; ctx.fill();
  var first = samples[0].t, last = samples[samples.length - 1].t;
  $("chartRange").textContent = samples.length + " samples · " +
    new Date(first * 1000).toLocaleTimeString() + " – " + new Date(last * 1000).toLocaleTimeString();
}
function render(d){
  lastLive = d;
  $("moistPct").textContent = d.moisturePct;
  var b = bandOf(d.moisturePct);
  var sEl = $("moistStatus"); sEl.textContent = b[0]; sEl.style.color = b[1];
  var arc = $("gaugeArc"); arc.style.stroke = b[1];
  arc.style.strokeDashoffset = (ARC * (1 - d.moisturePct / 100)).toFixed(2);
  $("rawVal").textContent = d.moistureRaw;
  $("tempVal").innerHTML = d.temperature.toFixed(1) + ' <span class="unit">°C</span>';
  $("humVal").innerHTML = Math.round(d.humidity) + ' <span class="unit">%</span>';
  var pv = $("pumpVal");
  pv.textContent = d.pumpOn ? "RUNNING" : "OFF";
  pv.className = "val" + (d.pumpOn ? " green" : "");
  $("pumpSub").textContent = d.pumpOn ? "pump is running" : "pump is stopped";
  var mv = $("modeVal");
  mv.textContent = d.mode === "auto" ? "AUTO" : "MANUAL";
  mv.className = "val" + (d.mode === "auto" ? " green" : " amber");
  $("modeSub").textContent = d.mode === "auto" ? "thresholds drive the pump" : "you drive the pump";
  if (!pending){
    $("motorOn").className = "on" + (d.pumpOn ? " active" : "");
    $("motorOff").className = "off" + (d.pumpOn ? "" : " active");
    $("segAuto").className = d.mode === "auto" ? "active" : "";
    $("segManual").className = d.mode === "auto" ? "" : "active";
  }
  var auto = d.mode === "auto";
  $("motorOn").disabled = auto;
  $("motorOff").disabled = auto;
  $("pumpNote").style.display = auto ? "block" : "none";
  $("cloudNote").textContent = "Board is online — readings arrive every 10 s from Firebase.";
  var now = Date.now();
  var lastSampleAt = samples.length ? samples[samples.length - 1].t * 1000 : 0;
  if (now - lastSampleAt >= 5000){
    samples.push({ t: Math.floor(now / 1000), v: d.moisturePct });
    if (samples.length > 50) samples.shift();
  }
  drawChart();
}
function sendControl(patch, btn, doneMsg){
  if (!controlRef){ toast("Not connected to Firebase yet."); return; }
  if (pending) return;
  pending = true;
  if (btn) btn.disabled = true;
  controlRef.update(patch).then(function(){
    pending = false;
    if (btn) btn.disabled = false;
    toast(doneMsg || "Sent — waiting for the board to confirm.");
  }).catch(function(e){
    pending = false;
    if (btn) btn.disabled = false;
    toast("Could not send — " + (e && e.message ? e.message : "Firebase error"));
  });
}
function initFirebase(){
  if (!configured()){
    $("setupBanner").style.display = "block";
    $("updText").textContent = "not configured";
    setStatus("setup");
    return;
  }
  if (typeof firebase === "undefined"){
    $("setupBanner").style.display = "block";
    $("setupBanner").innerHTML = "<strong>Firebase SDK did not load.</strong><br>Check your internet connection and reload the page.";
    setStatus("setup");
    return;
  }
  firebase.initializeApp(FIREBASE_CONFIG);
  db = firebase.database();
  setStatus("offline");  // configured, but no board data yet
  $("updText").textContent = "waiting for board…";
  controlRef = db.ref("agriflow/control");
  liveRef = db.ref("agriflow/live");
  histRef = db.ref("agriflow/history");

  db.ref(".info/connected").on("value", function(snap){
    online = snap.val() === true;
    if (!online) setStatus("offline");
    else if (lastOk) setStatus("live");
  });
  liveRef.on("value", function(snap){
    var d = snap.val();
    if (!d) return;
    lastOk = Date.now();
    setStatus("live");
    render(d);
  });
  histRef.orderByKey().limitToLast(50).on("value", function(snap){
    var arr = [];
    snap.forEach(function(child){
      var v = child.val();
      if (v && typeof v.moisturePct === "number" && typeof v.t === "number")
        arr.push({ t: v.t, v: v.moisturePct });
    });
    if (arr.length){ samples = arr.slice(-50); drawChart(); }
  });
  // Keep the threshold display in sync with /control (e.g. if the board
  // or another client changes them). Inputs are left alone while focused.
  controlRef.on("value", function(snap){
    var c = snap.val(); if (!c) return;
    if (typeof c.onThreshold === "number" && typeof c.offThreshold === "number"){
      dryBelow = c.onThreshold; wetAbove = c.offThreshold;
      if (document.activeElement !== $("onBelow")) $("onBelow").value = dryBelow;
      if (document.activeElement !== $("offAbove")) $("offAbove").value = wetAbove;
      $("footDry").textContent = dryBelow; $("footWet").textContent = wetAbove;
      positionTick(); drawChart();
    }
  });
  // Seed the control node with the current thresholds on first load so the
  // board and the dashboard agree (only if the node is empty).
  controlRef.once("value").then(function(snap){
    if (!snap.exists()){
      controlRef.set({ pump: "off", mode: "auto", onThreshold: 30, offThreshold: 50 });
    } else {
      var c = snap.val() || {};
      if (typeof c.onThreshold === "number") dryBelow = c.onThreshold;
      if (typeof c.offThreshold === "number") wetAbove = c.offThreshold;
      $("onBelow").value = dryBelow; $("offAbove").value = wetAbove;
      $("footDry").textContent = dryBelow; $("footWet").textContent = wetAbove;
      positionTick();
    }
  });
}
$("segAuto").addEventListener("click", function(){
  sendControl({ mode: "auto" }, $("segAuto"), "Mode set to AUTO — sent to the board.");
});
$("segManual").addEventListener("click", function(){
  sendControl({ mode: "manual" }, $("segManual"), "Mode set to MANUAL — sent to the board.");
});
$("motorOn").addEventListener("click", function(){
  if (lastLive && lastLive.mode === "auto"){ toast("Auto mode is driving the pump — switch to Manual first."); return; }
  sendControl({ pump: "on" }, $("motorOn"), "Pump ON sent — waiting for the board to confirm.");
});
$("motorOff").addEventListener("click", function(){
  if (lastLive && lastLive.mode === "auto"){ toast("Auto mode is driving the pump — switch to Manual first."); return; }
  sendControl({ pump: "off" }, $("motorOff"), "Pump OFF sent — waiting for the board to confirm.");
});
$("applyThresh").addEventListener("click", function(){
  var lo = parseInt($("onBelow").value, 10), hi = parseInt($("offAbove").value, 10);
  if (isNaN(lo) || isNaN(hi) || lo < 0 || hi > 100 || lo >= hi){
    toast("Thresholds must be 0–100 with ON-below smaller than OFF-above.");
    return;
  }
  dryBelow = lo; wetAbove = hi;
  $("footDry").textContent = lo; $("footWet").textContent = hi;
  positionTick(); drawChart();
  sendControl({ onThreshold: lo, offThreshold: hi }, $("applyThresh"),
    "Thresholds " + lo + "% / " + hi + "% sent to the board.");
});
setInterval(function(){
  if (!lastOk){
    if (configured() && typeof firebase === "undefined") $("updText").textContent = "SDK did not load";
    else $("updText").textContent = configured() ? "waiting for board…" : "not configured";
    return;
  }
  var s = Math.round((Date.now() - lastOk) / 1000);
  $("updText").textContent = fmtAgo(s);
  if (s > 30 && online !== false) setStatus("offline");
}, 1000);
window.addEventListener("resize", drawChart);
positionTick();
drawChart();
initFirebase();
