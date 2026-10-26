# AGRIFLOW — V3.0 Changes (vs V2.6)

**One line:** V2.6 behaviour, plus the cloud. Same hardware, wiring, pins
and calibration. The board joins a phone hotspot, pushes readings to
Firebase, and takes pump/mode commands from a cloud dashboard.

## What is new in V3.0

- **WiFi STA mode:** the board joins the phone hotspot (SSID/password at
  the top of the sketch). If the hotspot is not reachable within ~20 s,
  it falls back to its own Access Point (`ESP32_Plant_System` /
  `12345678`) exactly like V2.6 — the local page at http://192.168.4.1
  keeps working. WiFi LED (GPIO2): solid ON = online, slow blink = AP-only.
- **Firebase push:** every 10 s the board PUTs live readings to
  `/agriflow/live` (`moisturePct, moistureRaw, temperature, humidity,
  pumpOn, mode, updatedAt`) over HTTPS REST (WiFiClientSecure, connection
  reuse, 3 s timeouts). A history sample is POSTed to `/agriflow/history`
  every 60 s.
- **Firebase control:** every 3 s the board GETs `/agriflow/control`
  (`pump, mode, onThreshold, offThreshold`) and applies it. MANUAL mode
  → the pump follows the cloud; AUTO mode → cloud pump commands are
  ignored (the board's own logic owns the pump, same rule as the local
  `/pump` route). Cloud thresholds override the 30/50 defaults.
- **Cloud dashboard** (`dashboard/`, deploys to Vercel): live moisture
  gauge, temperature/humidity/pump/mode cards, moisture history chart
  (last 50 samples), pump ON/OFF, AUTO/MANUAL switch, threshold inputs.
  It only ever shows state the board confirmed via `/live`; if the board
  stops pushing for 30 s it switches to OFFLINE.
- **`/data` gains two fields:** `online` (hotspot connected) and
  `cloudOk` (last Firebase round-trip succeeded). The local page ignores
  fields it does not read, so this is backwards compatible.
- **New library:** ArduinoJson (parses `/control`).

## What did NOT change

Pins, calibration (wet 1050 / dry 3050), AUTO thresholds ON <30% / OFF
>50%, sensor fault ONLY on raw pinned at 0 (V2.6 fix kept), pump failsafe
(120 s max run / 60 s cooldown), timed watering, OLED layout + event
flash, buzzer/buttons/LEDs, local routes and JSON API, non-blocking
buzzer, DHT-failure keeps last good values.

## Fixes found during the V3 build

- The V2 preview stub still simulated the OLD 20/80 thresholds (the
  firmware moved to 30/50 in V2.5 but the stub was never updated), so the
  local preview showed the wrong thresholds. Fixed in V3: stub now
  simulates 30/50 and the new `online`/`cloudOk` fields.
- Dashboard status pill could stay stuck on "SETUP" with a real config
  but no board data yet. Fixed: it now shows OFFLINE + "waiting for
  board…" until the first reading arrives, and a distinct message if the
  Firebase SDK itself fails to load.

## Verification (25 Oct 2026)

- `build.py` OK; `build/verify.py` ALL PASS (pins, thresholds, fault
  rule, routes, JSON contract, Firebase paths/keys/intervals, JS syntax).
- Firmware ↔ dashboard key cross-check ALL PASS.
- Dashboard served locally (all files HTTP 200) and rendered headless;
  local preview rendered LIVE at 33% DRY with correct 30/50 footer.
- Firebase live smoke test PASS (GET/PUT/DELETE round-trip on the real
  database; left clean). Test-mode rules expire **2026-11-24** — see README.
- NOT compiled here (no Arduino toolchain in this VM) — Tim compiles in
  the Arduino IDE, as with V2.6. NOT deployed yet — Vercel deploy is the
  parent's next step.
