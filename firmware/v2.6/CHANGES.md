# AGRIFLOW — V1 → V2 Changes

**One line:** V2 keeps the exact same hardware, wiring, pins and calibration as V1.
Only the *software logic* and the *web page* were rebuilt, because V1 had real bugs
in how the buttons and the pump state worked.

---

## The bugs in V1 — and how V2 fixes them

### Bug 1 — The web page buttons looked dead
**What happened:** You tap "Toggle Pump", the buzzer beeps, but the page keeps
showing the old values. The page was a fixed photo (HTML) made once when it
loaded; the buttons sent a signal but the answer was thrown away. Only the
"Refresh" button (a full page reload) showed the truth.

**V2 fix:** The device now has a proper JSON API. `GET /data` returns every live
value, and every button command returns the *confirmed* new state. The page
updates itself every 2 seconds and repaints from the device's answer — never
from a guess. There is no Refresh button anymore, because there is nothing
stale to refresh.

### Bug 2 — In Auto mode, the pump button silently did nothing
**What happened:** `handleToggle()` checked `if (!prevMode)` — in Auto mode the
command was skipped, but the code still beeped for 500 ms and replied
"Pump toggled". The user heard a success beep for something that never
happened. The physical pump button had the same silent behaviour.

**V2 fix:** A pump command in Auto mode now changes nothing **and says so**:
the API returns `"err":"auto-controls-pump"`, the web page shows the message
*"Auto mode controls the pump — switch to Manual first"*, the pump button is
visibly dimmed in Auto mode with that reason printed under it, and pressing
the physical button in Auto mode just logs "ignored" on Serial instead of
faking success.

### Bug 3 — Physical buttons called the web-server functions
**What happened:** The button handlers called `handleToggle()` / `handleMode()`,
which end with `server.send(...)` — sending an HTTP reply when **no HTTP
request exists**. That can corrupt a real reply happening at the same moment
and is simply the wrong layer: a hardware button is not a web request.

**V2 fix:** Buttons now call plain internal functions (`doTogglePump()`,
`doToggleMode()`). `server.send()` exists only inside real HTTP handlers.

### Bug 4 — Every beep froze the whole system for half a second
**What happened:** Beeps used `delay(500)` (and `delay(1000)` at startup).
During a delay the single loop is stuck — the web server cannot answer, so
pages timed out and the device felt broken right after every button press.

**V2 fix:** The buzzer is a non-blocking service: `beep(ms)` only schedules
the sound, and `buzzerService()` in `loop()` switches it off when the time is
up. The server never waits. The manual-mode low/high-moisture buzzer alert
works exactly as in V1, also non-blocking.

### Bug 5 — A failed DHT11 read displayed a fake 0 °C / 0 %
**What happened:** When the DHT read failed (NaN), V1 stored `0` into the
temperature/humidity variables, so the dashboard and OLED happily showed
0 °C and 0 % — a lie that looks like data.

**V2 fix:** On failure the last *good* values are kept and the API reports
`"dhtOk":false`. The page then shows "--" with the note *"DHT11 not
responding — last good reading shown"* (the OLED keeps showing the last good
values, as before).

### Bug 6 — Two different "truths" about the pump
**What happened:** V1 tracked the pump in a variable (`toggleRelay`) *and*
read the relay pin directly in different places. The auto logic, the buzzer
and the toggle code each kept their own version, so they could disagree.

**V2 fix:** One source of truth: `bool pumpOn` and `bool autoMode`. The only
way to change the pump is `setPump()`, which writes the pin and the variable
together; mode changes go through `doSetMode()` (which also drives the Mode
LED). Dead code was removed (the unused `currMode` string, and the declared
but never-defined `handleEvent1/2` prototypes).

---

## What is new in V2

- **Live mobile dashboard** — one inline page (16 KB, no internet needed,
  works on the ESP32's own WiFi): sticky header with a LIVE/OFFLINE pill and
  "updated Xs ago", a semicircular moisture gauge coloured by 5 bands
  (Critical <20 · Dry 20–39 · Ideal 40–70 · Moist 71–85 · Saturated 86–100),
  a history sparkline (last 10 minutes of this session), temperature and
  humidity cards, and sticky thumb-zone controls.
- **Dark + light themes** — the page follows the phone's
  `prefers-color-scheme`; the light theme stays readable in garden sunlight.
- **Connection honesty** — if the device stops answering for more than
  6 seconds, the page switches to OFFLINE: values dim, controls disable,
  and it keeps retrying. Buttons show a pending state, vibrate briefly on
  tap, and only ever display the state the device confirmed; on a timeout
  they revert and explain with a toast.
- **Weather card** — the phone's browser fetches current weather for
  Amaravati / VIT-AP (16.5062, 80.6480) from the free Open-Meteo API
  (no key): temperature, feels-like, condition, humidity, wind and rain.
  The last good weather is cached for 3 hours; with no internet the card
  collapses to one quiet line. *(Geolocation is deliberately NOT used —
  the page is served over plain HTTP, which is not a secure context, so
  the browser would refuse it on the real device.)*
- **Thresholds can never drift** — `/data` also sends `dryBelow` (20) and
  `wetAbove` (80) from the same firmware variables the AUTO logic uses, so
  the gauge tick, the history line and the footer always match the real
  watering behaviour.
- **WiFi LED (GPIO2)** is now actually driven (V1 defined it but never used
  it): ON once the Access Point is up.
- The old `/toggle` and `/refresh` addresses still work, so nothing that
  referenced them breaks.

## What did NOT change

Pins (moisture 34, DHT11 14, pump 25, buzzer 26, buttons 32/33, LEDs 2/15,
OLED 0x3C), calibration (wet 1050, dry 3050), AUTO thresholds (ON below 20%,
OFF above 80%), 2-second sensor/OLED refresh, AP name/password
(`ESP32_Plant_System` / `12345678`), the OLED layout, and the library list.

## Files

- `Code_ESP32_Webserver_Plant_Watering_V2.ino` — upload this to the ESP32.
- `preview.html` — the identical dashboard with a simulated device behind
  it; open it in a browser to see the design without hardware (the weather
  in the preview is real).
- `build/` — the sources the two files above are generated from
  (`page_template.html` is the single copy of the page; run
  `python3 build/build.py` after editing it).

## One honest note

This code was syntax- and structure-checked, but it was **not compiled here**
(no Arduino toolchain in this environment). Compile it once in the Arduino
IDE (ESP32 board package + the four libraries listed in the sketch header)
before the demo — that is the normal workflow for this project.

## V2.1 fix (23 Oct, after first Arduino IDE compile)
- Error was: `beep was not declared in this scope` (lines 434/446). Cause: `beep()` was defined below its first use, and the IDE automatic prototype generation got confused by the large web-page string — a known Arduino quirk, not a logic bug. Fix: explicit forward declarations for ALL functions added above the code. No behaviour changed.

## V2.2 (23 Oct, on Tim's hardware feedback)
- MOTOR control is now explicit: a dedicated Motor card with ON | OFF buttons (no more single toggle button). In AUTO the pair is dimmed and tapping it says why + flashes the MODE switch. Motor RUNNING shows a pulsing green state pill and a soft glow on the card. Firmware endpoints unchanged (/pump?state=on|off).
- Moisture-history fix: sparkline points now stretch across the full width as they collect (before, a fresh session drew only in the left corner and looked empty) + a dot marks the latest point, so even a flat 0% line is clearly visible. (Flat at 0% still means raw > 3050: probe in air / bone-dry soil.)
- Build system: the V2.1 forward-declarations fix now lives in build/firmware_tail.cppfrag (source of truth), so rebuilds keep it; verify.py handler-slice regex fixed to match definitions, not prototypes.

## V2.3 (23 Oct, Tim's hardware feedback #2)
- OLED event flash: mode change or motor on/off now shows full-screen on the OLED ("MOTOR ON/OFF", "MODE AUTO/MANUAL") for ~2 s, then the normal screen returns. Hooked into setPump()/doSetMode(), so web buttons, physical buttons and auto-mode pump changes all flash. Non-blocking (millis window + loop restore); updateOLED() skips while a flash is up.
- Web MOTOR ON|OFF buttons were already added in V2.2 — Tim's board was still running the V2.1 upload; V2.3 carries both.

## V2.4 (23 Oct, Tim approved all three gap-fixes)
- PUMP FAILSAFE: max continuous run 120 s -> forced stop + 60 s cooldown (page shows a rest note, OLED flashes FAILSAFE STOPPED, starts refused with err "failsafe-rest"). SENSOR FAULT: raw pinned at 0/4095 for 3 samples (6 s) -> pump stopped, AUTO suspended, OLED "SENSOR FAULT / CHECK PIN", red banner on the page; recovers by itself when readings look sane.
- TIMED WATERING: "Water now" button (MANUAL only) -> /water?sec=10 (clamped 5-60 s); pumpGuardService() stops it on time; any manual ON/OFF cancels the timer; button shows the countdown.
- STATS: firmware counts pump runs / total run time / last run (since boot); JSON fields runs, pumpSec, lastSec (+ fault, coolSec, timedSec); page gained a "Pump Record" card. Stats reset on reboot (NVS persistence was gap #4, not in this round).

---

## V2.5 — AUTO motor not turning off in water (Tim's report, 24 Oct 2026)

**Symptom:** AUTO mode, motor running, sensor dipped into water — motor kept running.

**Root cause (not a logic bug):** the AUTO OFF condition itself was correct
(`moisturePercentage > moistPerHigh` → `setPump(false)`, checked every loop),
but OFF needed **80%**, and 80% on this calibration (dry 3050 / wet 1050)
means a raw reading under **1450**. The probe dipped in open water reads
higher than the wet-soil calibration point, so the percentage never crossed
80 and the OFF never fired.

**Fix:** thresholds lowered — pump ON below **30%**, OFF above **50%**
(raw under ~2050). A water dip now crosses the OFF line easily; genuinely
wet soil still crosses it too. Page footer/JS defaults updated to 30/50 to
match (the page also live-reads the real values from `/data`).
Verified: build.py OK, verify.py ALL PASS, thresholds + AUTO OFF line
grep-confirmed in the generated .ino, map math simulated for raw
1200–3000 (2000 → 52% → OFF; 2600 → 22% → ON).

**Retest:** upload V2.5, AUTO mode, start the motor with the probe in dry
air/soil, then dip the probe in water — motor should stop within ~2–4 s and
the OLED should flash "MOTOR OFF". If it ever misbehaves again, note the
RAW number the page shows in water — that number sets the exact threshold.

---

## V2.6 — SENSOR FAULT always showing in AUTO (Tim's photo, 24 Oct 2026)

**Symptom:** OLED permanently showed "SENSOR FAULT / CHECK PIN" in AUTO
while the probe was out of the soil; dipping the probe made it work.

**Root cause:** this probe reads ~4095 (the ADC top rail) in open air —
a genuine bone-dry reading. V2.4's fault detector treated BOTH rails
(0 and 4095) as faults, so plain air tripped it after 3 samples.

**Fix:** fault now trips ONLY on raw pinned at 0 (a real signal short to
ground). 4095 in air maps to 0% moisture as it should. Trade-off, stated
plainly: at the top rail, "probe in air" and "wire off" look identical on
this probe, so CHECK PIN can no longer flag the wire-off case — correct
behaviour in air was worth more. Build + verify ALL PASS; the corrected
condition grep-confirmed in the generated .ino.
