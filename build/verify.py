#!/usr/bin/env python3
"""AGRIFLOW V3 verification (no Arduino toolchain in this VM, so:
 structure + JS syntax + API contract + Firebase contract checks instead
 of a full compile). Tim compiles the .ino once in the Arduino IDE —
 that is the normal workflow for this project."""
import pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
ino = (ROOT / "firmware" / "Code_ESP32_Plant_Watering_V3.ino").read_text(encoding="utf-8")
preview = (ROOT / "preview.html").read_text(encoding="utf-8")
fails = []
def check(name, ok, extra=""):
    print(("PASS " if ok else "FAIL ") + name + ((" — " + extra) if extra else ""))
    if not ok: fails.append(name)

# ---- extract firmware page from the raw literal ----
m = re.search(r'R"AGRIFLOW\(\n(.*?)\)AGRIFLOW"', ino, re.S)
check("page raw literal found", m is not None)
page = m.group(1)
check("firmware page size <= 25KB", len(page.encode()) <= 25*1024, f"{len(page.encode())} bytes")
check("firmware page has no preview stub", "simJson" not in page and "__PREVIEW_STUB__" not in page)
check("preview contains stub + same page", "simJson" in preview and "Moisture History" in preview)

# ---- strip strings/comments/raw literals for structural checks ----
def strip_code(src):
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith('R"AGRIFLOW(', i):
            j = src.index(')AGRIFLOW"', i); i = j + 10; out.append(" ")
        elif src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i); i = n if j < 0 else j + 2
        elif src[i] == '"':
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
            i += 1; out.append('""')
        elif src[i] == "'":
            i += 1
            while i < n and src[i] != "'":
                i += 2 if src[i] == "\\" else 1
            i += 1; out.append("''")
        else:
            out.append(src[i]); i += 1
    return "".join(out)

code = strip_code(ino)
for a, b in [("{","}"), ("(",")"), ("[","]")]:
    check(f"balance {a}{b}", code.count(a) == code.count(b), f"{code.count(a)} vs {code.count(b)}")

# delay() is allowed ONLY inside setup() (the 20 s hotspot wait). Anywhere
# else it would freeze the web server / cloud sync.
setup_m = re.search(r"void setup\(\) \{(.*?)\n\}", code, re.S)
setup_body = setup_m.group(1) if setup_m else ""
code_no_setup = code.replace(setup_body, "", 1) if setup_body else code
check("no delay() outside setup()", "delay(" not in code_no_setup)
check("setup() has the hotspot wait", "delay(250)" in setup_body)

for ghost in ["handleEvent1", "handleEvent2", "currMode", "toggleRelay", "prevMode", "temperature1", "humidity1", "controlBuzzer"]:
    check(f"V1 leftover '{ghost}' gone", ghost not in ino)
for route in ['"/"', '"/data"', '"/pump"', '"/mode"', '"/toggle"', '"/refresh"', '"/water"']:
    check(f"route {route} registered", f"server.on({route}" in ino)
senders = [fn for fn, body in re.findall(r"void (\w+)\(\) \{([^]]*?)\n\}", ino, re.S) if "server.send" in body]
check("server.send only in sendJson/handleRoot", set(senders) <= {"sendJson", "handleRoot"}, str(senders))
btn = re.search(r"void button1Handler\([^)]*\)\s*\{\n.*?\n\}", ino, re.S).group(0) + re.search(r"void button2Handler\([^)]*\)\s*\{\n.*?\n\}", ino, re.S).group(0)
check("button handlers contain no server. calls", "server." not in btn)
check("button handlers call internal toggles", "doTogglePump()" in btn and "doToggleMode()" in btn)
check("setPump is the only RelayPin writer", ino.count("digitalWrite(RelayPin") == 1, f"{ino.count('digitalWrite(RelayPin')} occurrence(s)")

# ---- V2.6 behaviour preserved ----
check("thresholds ON 30 / OFF 50", "moistPerLow = 30" in ino and "moistPerHigh = 50" in ino)
check("fault trips ONLY on raw 0", "sensorVal <= 0" in ino and "sensorVal >= 4095" not in ino)
check("calibration wet 1050 / dry 3050", "wetSoilVal = 1050" in ino and "drySoilVal = 3050" in ino)
check("failsafe 120 s max run / 60 s cooldown", "PUMP_MAX_RUN_MS = 120000" in ino and "PUMP_COOLDOWN_MS = 60000" in ino)
check("AP fallback SSID/password kept", '"ESP32_Plant_System"' in ino and '"12345678"' in ino)
for pin_def in ["SensorPin 34", "DHTPin 14", "RelayPin 25", "wifiLed 2",
                "RelayButtonPin 32", "ModeSwitchPin 33", "BuzzerPin 26", "ModeLed 15"]:
    check(f"pin {pin_def} unchanged", pin_def in ino)

# ---- V3 cloud contract ----
for lib in ["<WiFi.h>", "<WebServer.h>", "<HTTPClient.h>", "<WiFiClientSecure.h>", "<ArduinoJson.h>"]:
    check(f"include {lib}", lib in ino)
check("hotspot SSID is a placeholder (no real credential)", 'HOTSPOT_SSID = "YOUR_HOTSPOT_SSID"' in ino)
check("hotspot password is a placeholder (no real credential)", 'HOTSPOT_PASS = "YOUR_HOTSPOT_PASSWORD"' in ino)
check("FIREBASE_HOST is the real project host",
      'FIREBASE_HOST = "agriflow-iot-default-rtdb.firebaseio.com"' in ino)
fb_lines = [l for l in ino.splitlines() if "firebaseio.com" in l]
check("no other firebase host anywhere in the sketch",
      all("agriflow-iot-default-rtdb.firebaseio.com" in l for l in fb_lines),
      f"{len(fb_lines)} line(s) mention firebaseio.com")
check("STA + AP mode", "WIFI_AP_STA" in ino and "WiFi.softAP(ssid, password)" in ino)
check("hotspot connect with 20 s timeout", "WiFi.begin(HOTSPOT_SSID, HOTSPOT_PASS)" in ino and "20000" in ino)
for path in ["/agriflow/live", "/agriflow/control", "/agriflow/history"]:
    check(f"Firebase path {path} used", f'"{path}"' in ino)
check("live push uses PUT", "firebasePut(\"/agriflow/live\"" in ino)
check("history push uses POST", "firebasePost(\"/agriflow/history\"" in ino)
check("control poll uses GET", "firebaseGet(\"/agriflow/control\"" in ino)
check("live interval 10 s", "CLOUD_PUSH_MS = 10000" in ino)
check("control interval 3 s", "CONTROL_POLL_MS = 3000" in ino)
check("history interval 60 s", "HISTORY_PUSH_MS = 60000" in ino)
for key in ["moisturePct", "moistureRaw", "temperature", "humidity", "pumpOn", "mode", "updatedAt"]:
    check(f"live JSON key {key}", f'\\"{key}\\"' in ino or f'"{key}"' in ino)
for key in ['"pump"', '"mode"', '"onThreshold"', '"offThreshold"']:
    check(f"control key {key} parsed", key in ino)
check("cloud sync runs in loop()", "cloudSyncService();" in ino)
check("cloud calls are no-ops when offline", "if (!wifiOnline) return;" in ino)
check("AUTO ignores cloud pump commands", 'if (p && !autoMode)' in ino)

# ---- JSON contract: firmware keys vs JS reads (local page) ----
fw_keys = set(re.findall(r'\\"(\w+)\\":', ino))
js_fields = set(re.findall(r"\bd\.(\w+)", page)) | set(re.findall(r"\bc\.(\w+)", page))
api_fields = {"moisture","raw","temp","hum","pump","auto","dhtOk","uptimeSec","dryBelow","wetAbove","err",
              "runs","pumpSec","lastSec","fault","coolSec","timedSec","online","cloudOk"}
check("firmware JSON has all API fields", api_fields <= fw_keys, "missing: " + str(api_fields - fw_keys))
js_api_reads = {f for f in js_fields if f in api_fields or f in ("err",)}
check("every API field JS reads exists in firmware JSON", js_api_reads <= fw_keys, "missing: " + str(js_api_reads - fw_keys))
check("stub JSON matches firmware fields", all(f in preview for f in api_fields))

# ---- node --check on every <script> in firmware page + preview ----
def scripts(html):
    return re.findall(r"<script>\n?(.*?)</script>", html, re.S)
all_js = [("firmware page", s) for s in scripts(page)] + [("preview", s) for s in scripts(preview)]
for tag, js in all_js:
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(js); path = f.name
    r = subprocess.run(["node", "--check", path], capture_output=True, text=True)
    check(f"node --check ({tag}, {len(js)} chars)", r.returncode == 0, r.stderr.strip().splitlines()[-1] if r.returncode else "")

print("\nRESULT:", "ALL PASS" if not fails else f"{len(fails)} FAILURES: {fails}")
sys.exit(1 if fails else 0)
