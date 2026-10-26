// AGRIFLOW preview stub — simulates the ESP32 JSON API in the browser.
// This code exists ONLY in preview.html. It is never compiled into the .ino.
(function(){
  var realFetch = window.fetch.bind(window);
  var sim = { moisture:34, temp:29.4, hum:61, pump:false, auto:true, dhtOk:true, start:Date.now(),
              runs:0, pumpSec:0, lastSec:0, fault:false, coolSec:0, timedSec:0, pumpSince:0, timedUntil:0,
              online:true, cloudOk:true };
  function simJson(err){
    var now = Date.now();
    if(sim.timedUntil && now>=sim.timedUntil){ sim.timedUntil=0; if(sim.pump){ sim.pump=false; } }
    var tot = sim.pumpSec + (sim.pump && sim.pumpSince ? Math.floor((now-sim.pumpSince)/1000) : 0);
    var j = { moisture:Math.round(sim.moisture),
              raw: Math.round(3050 - (sim.moisture/100)*(3050-1050)),
              temp:+sim.temp.toFixed(1), hum:+sim.hum.toFixed(1),
              pump:sim.pump, auto:sim.auto, dhtOk:sim.dhtOk,
              uptimeSec: Math.floor((now-sim.start)/1000),
              dryBelow:30, wetAbove:50,
              runs:sim.runs, pumpSec:tot, lastSec:sim.lastSec, fault:sim.fault,
              coolSec:sim.coolSec, online:sim.online, cloudOk:sim.cloudOk,
              timedSec: sim.timedUntil ? Math.max(0,Math.round((sim.timedUntil-now)/1000)) : 0 };
    if(err) j.err = err;
    return j;
  }
  function drift(){
    sim.moisture += (Math.random()-0.48)*3 + (sim.pump ? 1.8 : -0.5);
    if(sim.moisture<4) sim.moisture=4; if(sim.moisture>96) sim.moisture=96;
    sim.temp += (Math.random()-0.5)*0.3;
    sim.hum  += (Math.random()-0.5)*1.2; if(sim.hum<30) sim.hum=30; if(sim.hum>90) sim.hum=90;
    if(sim.auto){ if(sim.moisture<30) sim.pump=true; if(sim.moisture>50) sim.pump=false; }
  }
  function reply(obj){ return Promise.resolve({ ok:true, json:function(){ return Promise.resolve(obj); } }); }
  window.fetch = function(url, opts){
    var u = String(url);
    if(u.indexOf("/data")===0){ drift(); return reply(simJson()); }
    if(u.indexOf("/pump")===0){
      if(sim.auto) return reply(simJson("auto-controls-pump"));
      var nowP=Date.now();
      function setP(on){ if(on&&!sim.pump){ sim.pump=true; sim.pumpSince=nowP; sim.runs++; } else if(!on&&sim.pump){ sim.pump=false; var ran=Math.floor((nowP-sim.pumpSince)/1000); sim.pumpSec+=ran; sim.lastSec=ran; sim.pumpSince=0; } sim.timedUntil=0; }
      if(u.indexOf("state=on")>-1) setP(true);
      else if(u.indexOf("state=off")>-1) setP(false);
      else setP(!sim.pump);
      return reply(simJson());
    }
    if(u.indexOf("/water")===0){
      if(sim.auto) return reply(simJson("auto-controls-pump"));
      var nowW=Date.now();
      if(!sim.pump){ sim.pump=true; sim.pumpSince=nowW; sim.runs++; }
      sim.timedUntil = nowW + 10000;
      return reply(simJson());
    }
    if(u.indexOf("/mode")===0){
      if(u.indexOf("state=auto")>-1) sim.auto=true;
      else if(u.indexOf("state=manual")>-1) sim.auto=false;
      else sim.auto=!sim.auto;
      return reply(simJson());
    }
    return realFetch(url, opts);  // weather API etc. go to the real internet
  };
})();
