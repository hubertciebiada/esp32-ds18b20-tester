/*
 * DS18B20 sensor tester for ESP32 (standalone, with web UI)
 * ---------------------------------------------------------
 * A workbench tester for dozens of DS18B20 sensors on a single 1-Wire bus.
 * It detects every sensor, shows their full ROM addresses and live
 * temperatures on a local web page, and helps identify which ROM belongs to
 * which physical sensor using the "warm glass" method (highlighting the
 * sensor with the highest temperature).
 *
 * The device is fully standalone - no Home Assistant / MQTT / ESPHome.
 *
 * IMPORTANT (reliability): the 1-Wire bus is touched ONLY from loop()
 * (the application core). The asynchronous web server handles requests in a
 * separate task and NEVER talks to the bus - it only serves state from
 * memory. This keeps the timing-critical 1-Wire operations from colliding
 * with networking.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// ====================== CONFIGURATION ======================
// All settings in one place, at the top of the file.

#define ONE_WIRE_BUS        4        // 1-Wire bus GPIO (DQ)
#define SENSOR_RESOLUTION   12       // sensor resolution: 9..12 bit (default 12)
#define MAX_SENSORS         50       // maximum number of sensors on the bus
#define READ_INTERVAL_MS    1000UL   // interval between measurement cycles

// --- WiFi: Access Point mode (default) ---
// NOTE: change the password before field use. The AP password must be at least 8 characters.
#define WIFI_AP_SSID        "DS18B20-Tester"
#define WIFI_AP_PASS        "tester1234"

// --- WiFi: STA mode (client of an existing network) ---
// To use STA mode instead of AP: uncomment the constants below AND set
// USE_WIFI_STA to 1. In STA mode read the IP address from the serial monitor.
#define USE_WIFI_STA        0
// #define WIFI_STA_SSID    "YourNetwork"
// #define WIFI_STA_PASS     "YourPassword"

// ==========================================================

// Status of a single sensor (read error diagnostics).
enum SensorStatus : uint8_t {
  ST_OK = 0,            // valid reading
  ST_RESET85,           // 85.00 °C - no successful conversion / weak power
  ST_DISCONNECTED,      // -127 °C - disconnected / no communication
  ST_NOREAD             // no reading / NaN
};

// A single sensor detected on the bus.
struct Sensor {
  uint8_t  addr[8];     // 8-byte ROM address (read order: family..CRC)
  float    tempC;       // last temperature (NAN if none)
  uint8_t  status;      // SensorStatus
};

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature dallas(&oneWire);
AsyncWebServer server(80);

Sensor   sensorList[MAX_SENSORS];
uint8_t  sensorCount = 0;
int      maxIndex    = -1;     // index of the hottest healthy sensor (-1 = none)

// State of the non-blocking measurement cycle (based on millis()).
bool          conversionPending = false;
unsigned long conversionStart   = 0;
unsigned long lastRequest       = 0;
volatile bool rescanRequested   = false;

// ---------- Helpers: ROM formatting ----------

// Full ROM in a human-readable format: 16 hex chars, UPPERCASE (e.g. 28FF641E0016035C).
void romReadable(const uint8_t *addr, char *out /* >=17 B */) {
  for (uint8_t i = 0; i < 8; i++) {
    sprintf(out + i * 2, "%02X", addr[i]);
  }
  out[16] = '\0';
}

// ROM in the variant ready to paste into ESPHome (the `address` field).
//
// Byte-order check: ESPHome treats the address as a 64-bit number in which the
// FAMILY CODE (0x28 for DS18B20) is the least significant byte. When written in
// hex (0x...), the bytes therefore appear in the REVERSE order compared to the
// bus read order - the family code lands at the end, the CRC at the start.
// Example: ROM 28 FF 64 1E 00 16 03 5C  ->  ESPHome 0x5c0316001e64ff28
void romEsphome(const uint8_t *addr, char *out /* >=19 B */) {
  out[0] = '0';
  out[1] = 'x';
  for (uint8_t i = 0; i < 8; i++) {
    sprintf(out + 2 + i * 2, "%02x", addr[7 - i]);  // bytes from least significant
  }
  out[18] = '\0';
}

// Likely-clone flag: a genuine DS18B20 has the ROM pattern
// 28-xx-xx-xx-xx-00-00-xx (family code 0x28, bytes 5 and 6 equal to 0x00).
// We do NOT reject the sensor - we only flag it visually.
bool isLikelyClone(const uint8_t *addr) {
  return !(addr[0] == 0x28 && addr[5] == 0x00 && addr[6] == 0x00);
}

const char *statusToString(uint8_t st) {
  switch (st) {
    case ST_OK:           return "ok";
    case ST_RESET85:      return "reset85";
    case ST_DISCONNECTED: return "disconnected";
    default:              return "noread";
  }
}

// Conversion time depends on resolution (DS18B20 datasheet).
unsigned long conversionDelayMs() {
  switch (SENSOR_RESOLUTION) {
    case 9:  return 94;
    case 10: return 188;
    case 11: return 375;
    default: return 750;   // 12-bit
  }
}

// ---------- Bus scan ----------

// Detects all sensors on the bus and stores their ROM addresses.
// Called at startup and on demand via /rescan (always from loop()).
void scanBus() {
  dallas.begin();

  uint16_t found = dallas.getDeviceCount();
  if (found > MAX_SENSORS) found = MAX_SENSORS;

  sensorCount = 0;
  for (uint16_t i = 0; i < found; i++) {
    DeviceAddress a;
    if (dallas.getAddress(a, i)) {
      memcpy(sensorList[sensorCount].addr, a, 8);
      sensorList[sensorCount].tempC  = NAN;
      sensorList[sensorCount].status = ST_NOREAD;
      sensorCount++;
    }
  }

  // Set the resolution globally and enable non-blocking mode.
  dallas.setResolution(SENSOR_RESOLUTION);
  dallas.setWaitForConversion(false);

  // Force an immediate first measurement after the scan.
  conversionPending = false;
  lastRequest = millis() - READ_INTERVAL_MS;
  maxIndex = -1;

  Serial.printf("[scan] sensors detected: %u (limit %u)\n", sensorCount, (unsigned)MAX_SENSORS);
}

// ---------- Measurement cycle (non-blocking) ----------

// Reads the scratchpads after the conversion finishes + classifies statuses.
void readAllTemperatures() {
  int    bestIdx  = -1;
  float  bestTemp = -1000.0f;

  for (uint8_t i = 0; i < sensorCount; i++) {
    float t = dallas.getTempC(sensorList[i].addr);

    if (t == DEVICE_DISCONNECTED_C) {            // -127 °C
      sensorList[i].tempC  = NAN;
      sensorList[i].status = ST_DISCONNECTED;
    } else if (isnan(t)) {
      sensorList[i].tempC  = NAN;
      sensorList[i].status = ST_NOREAD;
    } else if (fabsf(t - 85.0f) < 0.01f) {       // 85.00 °C = power-on reset
      sensorList[i].tempC  = t;
      sensorList[i].status = ST_RESET85;
    } else {
      sensorList[i].tempC  = t;
      sensorList[i].status = ST_OK;
      if (t > bestTemp) { bestTemp = t; bestIdx = i; }  // only ST_OK counts toward the maximum
    }
  }

  maxIndex = bestIdx;  // hottest HEALTHY sensor (faulty ones excluded)
}

// ---------- JSON / CSV ----------

// Builds JSON with the state of all sensors (for the /data endpoint).
String buildJson() {
  JsonDocument doc;
  doc["count"]      = sensorCount;
  doc["resolution"] = SENSOR_RESOLUTION;
  doc["maxIndex"]   = maxIndex;

  char rom[17];
  if (maxIndex >= 0) {
    romReadable(sensorList[maxIndex].addr, rom);
    doc["maxRom"]  = rom;
    doc["maxTemp"] = sensorList[maxIndex].tempC;
  } else {
    doc["maxRom"]  = nullptr;
    doc["maxTemp"] = nullptr;
  }

  JsonArray arr = doc["sensors"].to<JsonArray>();
  char esph[19];
  for (uint8_t i = 0; i < sensorCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    romReadable(sensorList[i].addr, rom);
    romEsphome(sensorList[i].addr, esph);
    o["rom"]     = rom;
    o["esphome"] = esph;
    o["status"]  = statusToString(sensorList[i].status);
    o["clone"]   = isLikelyClone(sensorList[i].addr);
    if (isnan(sensorList[i].tempC)) {
      o["tempC"] = nullptr;
    } else {
      o["tempC"] = sensorList[i].tempC;
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// Builds a CSV file with the sensor state (for the /export.csv endpoint).
String buildCsv() {
  String csv = F("index;rom;esphome;tempC;status;clone\n");
  char rom[17];
  char esph[19];
  for (uint8_t i = 0; i < sensorCount; i++) {
    romReadable(sensorList[i].addr, rom);
    romEsphome(sensorList[i].addr, esph);
    csv += String(i);
    csv += ';'; csv += rom;
    csv += ';'; csv += esph;
    csv += ';';
    if (isnan(sensorList[i].tempC)) csv += "";
    else                            csv += String(sensorList[i].tempC, 2);
    csv += ';'; csv += statusToString(sensorList[i].status);
    csv += ';'; csv += (isLikelyClone(sensorList[i].addr) ? "1" : "0");
    csv += '\n';
  }
  return csv;
}

// ---------- Web page (in PROGMEM) ----------

const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DS18B20 Tester</title>
<style>
  :root{--bg:#0f172a;--card:#1e293b;--mut:#94a3b8;--ok:#22c55e;--warn:#f59e0b;
        --err:#ef4444;--hot:#f97316;--line:#334155;--fg:#e2e8f0;}
  *{box-sizing:border-box}
  body{margin:0;background:var(--bg);color:var(--fg);
       font-family:system-ui,Segoe UI,Roboto,sans-serif;font-size:15px}
  header{padding:16px 20px;border-bottom:1px solid var(--line);
         display:flex;align-items:center;gap:16px;flex-wrap:wrap}
  h1{font-size:18px;margin:0}
  .pill{background:var(--card);border:1px solid var(--line);border-radius:999px;
        padding:4px 12px;font-size:13px;color:var(--mut)}
  .pill b{color:var(--fg)}
  .wrap{padding:20px;max-width:1100px;margin:0 auto}
  .hot{background:linear-gradient(90deg,#7c2d12,#9a3412);border:1px solid var(--hot);
       border-radius:12px;padding:16px 20px;margin-bottom:18px}
  .hot .lbl{color:#fed7aa;font-size:13px;text-transform:uppercase;letter-spacing:.05em}
  .hot .rom{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:22px;
            font-weight:700;margin-top:4px;word-break:break-all}
  .hot .t{font-size:30px;font-weight:800;margin-top:6px}
  .hot.empty{background:var(--card);border-color:var(--line)}
  .hot.empty .rom,.hot.empty .t{color:var(--mut);font-weight:600;font-size:16px}
  .bar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px}
  button{background:var(--card);color:var(--fg);border:1px solid var(--line);
         border-radius:8px;padding:9px 16px;font-size:14px;cursor:pointer}
  button:hover{border-color:var(--mut)}
  button.primary{background:#2563eb;border-color:#2563eb}
  table{width:100%;border-collapse:collapse;background:var(--card);
        border:1px solid var(--line);border-radius:10px;overflow:hidden}
  th,td{padding:10px 12px;text-align:left;border-bottom:1px solid var(--line)}
  th{font-size:12px;text-transform:uppercase;letter-spacing:.04em;color:var(--mut)}
  tr:last-child td{border-bottom:none}
  td.rom{font-family:ui-monospace,Menlo,Consolas,monospace}
  td.temp{font-variant-numeric:tabular-nums;font-weight:700;white-space:nowrap}
  tr.hotrow{background:rgba(249,115,22,.18);outline:2px solid var(--hot)}
  .badge{display:inline-block;padding:2px 9px;border-radius:999px;font-size:12px;
         font-weight:600}
  .b-ok{background:rgba(34,197,94,.15);color:var(--ok)}
  .b-reset85{background:rgba(245,158,11,.15);color:var(--warn)}
  .b-disconnected{background:rgba(239,68,68,.15);color:var(--err)}
  .b-noread{background:rgba(148,163,184,.15);color:var(--mut)}
  .clone{margin-left:6px;background:rgba(245,158,11,.15);color:var(--warn);
         border:1px solid var(--warn);padding:1px 7px;border-radius:999px;font-size:11px}
  .toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%);
         background:#2563eb;color:#fff;padding:10px 18px;border-radius:8px;
         opacity:0;transition:opacity .2s;pointer-events:none}
  .toast.show{opacity:1}
  .empty-tbl{text-align:center;color:var(--mut);padding:24px}
</style>
</head>
<body>
<header>
  <h1>🌡️ DS18B20 Sensor Tester</h1>
  <span class="pill">Sensors: <b id="count">–</b></span>
  <span class="pill">Resolution: <b id="res">–</b> bit</span>
  <span class="pill">Status: <b id="conn">connecting…</b></span>
</header>

<div class="wrap">
  <div id="hotbox" class="hot empty">
    <div class="lbl">Hottest sensor (warm glass method)</div>
    <div class="rom" id="hotrom">– no data –</div>
    <div class="t" id="hottemp"></div>
  </div>

  <div class="bar">
    <button class="primary" onclick="rescan()">🔄 Rescan</button>
    <button onclick="exportCsv()">⬇️ Export CSV</button>
    <button onclick="copyRoms()">📋 Copy ROM list</button>
  </div>

  <table>
    <thead>
      <tr><th>#</th><th>ROM</th><th>ESPHome</th><th>Temperature</th><th>Status</th></tr>
    </thead>
    <tbody id="tbody">
      <tr><td colspan="5" class="empty-tbl">Loading…</td></tr>
    </tbody>
  </table>
</div>

<div id="toast" class="toast"></div>

<script>
// State kept only in JS memory (no localStorage/sessionStorage).
let latest = {sensors: []};

const STLABEL = {ok:'OK', reset85:'Reset 85°C', disconnected:'Disconnected', noread:'No reading'};

function fmtTemp(s){
  if(s.tempC === null || s.status === 'disconnected' || s.status === 'noread') return '—';
  return s.tempC.toFixed(2) + ' °C';
}

function render(d){
  document.getElementById('count').textContent = d.count;
  document.getElementById('res').textContent   = d.resolution;

  const hb = document.getElementById('hotbox');
  if(d.maxIndex >= 0 && d.maxRom){
    hb.classList.remove('empty');
    document.getElementById('hotrom').textContent  = d.maxRom;
    document.getElementById('hottemp').textContent = d.maxTemp.toFixed(2) + ' °C';
  } else {
    hb.classList.add('empty');
    document.getElementById('hotrom').textContent  = '– no healthy sensors –';
    document.getElementById('hottemp').textContent = '';
  }

  const tb = document.getElementById('tbody');
  if(!d.sensors.length){
    tb.innerHTML = '<tr><td colspan="5" class="empty-tbl">No sensors detected. Check the wiring and pull-up.</td></tr>';
    return;
  }
  let html = '';
  d.sensors.forEach((s,i)=>{
    const hot   = (i === d.maxIndex) ? ' class="hotrow"' : '';
    const clone = s.clone ? '<span class="clone" title="ROM does not match the 28-xx-xx-xx-xx-00-00-xx pattern">clone?</span>' : '';
    html += `<tr${hot}>
      <td>${i}</td>
      <td class="rom">${s.rom}${clone}</td>
      <td class="rom">${s.esphome}</td>
      <td class="temp">${fmtTemp(s)}</td>
      <td><span class="badge b-${s.status}">${STLABEL[s.status]||s.status}</span></td>
    </tr>`;
  });
  tb.innerHTML = html;
}

async function refresh(){
  try{
    const r = await fetch('/data', {cache:'no-store'});
    const d = await r.json();
    latest = d;
    render(d);
    document.getElementById('conn').textContent = 'connected';
  }catch(e){
    document.getElementById('conn').textContent = 'no connection';
  }
}

function rescan(){
  fetch('/rescan', {method:'POST'}).then(()=>toast('Rescan scheduled')).catch(()=>{});
}
function exportCsv(){ window.location = '/export.csv'; }

function copyRoms(){
  const txt = latest.sensors.map(s=>s.rom).join('\n');
  if(!txt){ toast('No sensors to copy'); return; }
  // navigator.clipboard requires a secure context - fallback for plain HTTP.
  if(navigator.clipboard && window.isSecureContext){
    navigator.clipboard.writeText(txt).then(()=>toast('ROM list copied')).catch(()=>fallbackCopy(txt));
  } else {
    fallbackCopy(txt);
  }
}
function fallbackCopy(txt){
  const ta = document.createElement('textarea');
  ta.value = txt; ta.style.position='fixed'; ta.style.opacity='0';
  document.body.appendChild(ta); ta.focus(); ta.select();
  try{ document.execCommand('copy'); toast('ROM list copied'); }
  catch(e){ toast('Copy failed'); }
  document.body.removeChild(ta);
}

let toastTimer;
function toast(msg){
  const t = document.getElementById('toast');
  t.textContent = msg; t.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(()=>t.classList.remove('show'), 1800);
}

refresh();
setInterval(refresh, 1000);  // auto-refresh every 1 s
</script>
</body>
</html>
)HTML";

// ---------- WiFi ----------

void setupWiFi() {
#if USE_WIFI_STA
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  Serial.print("[wifi] STA, connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(250);            // only in setup() - no delay() in loop()
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[wifi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[wifi] STA failed - check the network credentials");
  }
#else
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS);
  Serial.printf("[wifi] AP \"%s\", IP: %s\n", WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());
#endif
}

// ---------- Web server ----------

void setupServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", buildJson());
  });

  server.on("/export.csv", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res = req->beginResponse(200, "text/csv", buildCsv());
    res->addHeader("Content-Disposition", "attachment; filename=ds18b20.csv");
    req->send(res);
  });

  // We only set a flag for the scan - the actual scan runs in loop() (the bus
  // is touched only from the application core, never from the web server task).
  server.on("/rescan", HTTP_ANY, [](AsyncWebServerRequest *req) {
    rescanRequested = true;
    req->send(200, "application/json", "{\"ok\":true,\"scheduled\":true}");
  });

  server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "text/plain", "404");
  });

  server.begin();
  Serial.println("[http] server started on port 80");
}

// ---------- setup / loop ----------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== DS18B20 Tester (ESP32) ===");

  setupWiFi();
  scanBus();
  setupServer();
}

void loop() {
  // Handle a scan scheduled via the /rescan endpoint.
  if (rescanRequested) {
    rescanRequested = false;
    scanBus();
  }

  unsigned long now = millis();

  if (conversionPending) {
    // Wait non-blockingly for the conversion to finish, then read the scratchpads.
    if (now - conversionStart >= conversionDelayMs()) {
      readAllTemperatures();
      conversionPending = false;
      lastRequest = now;
    }
  } else {
    // Every READ_INTERVAL_MS trigger one shared conversion (Skip ROM + Convert T).
    if (now - lastRequest >= READ_INTERVAL_MS) {
      dallas.requestTemperatures();   // non-blocking (setWaitForConversion(false))
      conversionStart = now;
      conversionPending = true;
    }
  }
}
