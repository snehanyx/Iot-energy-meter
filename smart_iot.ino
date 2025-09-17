#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <PZEM004Tv30.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <ArduinoJson.h>

/* ====== CONFIG ====== */
const char* WIFI_SSID = "Redmi Note 9";
const char* WIFI_PASS = "887654321";

const int PZEM_RX = 16;  // ESP32 GPIO16 <- PZEM TX
const int PZEM_TX = 17;  // ESP32 GPIO17 -> PZEM RX

const unsigned long SAMPLE_INTERVAL_MS = 1000UL;   // read every 1s
const unsigned long SAVE_INTERVAL_MS   = 30UL*1000UL; // save every 30s
const uint8_t POWER_AVG_WINDOW = 5;  // smoothing window (samples)
const float STANDBY_THRESHOLD_W = 2.0; // for reference

float tariffRpKwh = 10.0; // ₹ per kWh (editable)

/* ====== FILES ====== */
const char* STATE_FILE = "/state.json";

/* ====== GLOBALS ====== */
WebServer server(80);
PZEM004Tv30* pzem = nullptr;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60 * 1000); // IST offset

// Energy bookkeeping
double daily_kwh = 0.0;
double yesterday_kwh = 0.0;
double month_kwh = 0.0;
double last_month_kwh = 0.0;
int lastDayNum = -1;
int month_id = -1;
int last_month_id = -1;

unsigned long lastSaveMs = 0;
unsigned long prevSampleTimeMs = 0;

// Power smoothing buffer
float p_buf[POWER_AVG_WINDOW] = {0};
uint8_t p_idx = 0;
bool p_filled = false;

// Manual Timer
unsigned long timerStartMs = 0;
unsigned long timerAccumulatedMs = 0;
bool timerRunning = false;

/* ====== HELPERS ====== */
// sanitize numbers to avoid garbage on UI
double safeNumber(double v) {
  if (isnan(v) || isinf(v)) return 0.0;
  // clamp to reasonable domain
  if (v < -1e8 || v > 1e8) return 0.0;
  return v;
}

int yyyymm_from_epoch(time_t epoch_local) {
  struct tm t;
  gmtime_r(&epoch_local, &t);
  return (t.tm_year + 1900) * 100 + (t.tm_mon + 1);
}

void saveState() {
  DynamicJsonDocument doc(1024);
  doc["daily_kwh"] = daily_kwh;
  doc["yesterday_kwh"] = yesterday_kwh;
  doc["month_kwh"] = month_kwh;
  doc["last_month_kwh"] = last_month_kwh;
  doc["tariff"] = tariffRpKwh;
  doc["lastDayNum"] = lastDayNum;
  doc["month_id"] = month_id;
  doc["last_month_id"] = last_month_id;
  // timer accum seconds
  doc["timer_accum_sec"] = (uint32_t)(timerAccumulatedMs / 1000UL);

  File f = LittleFS.open(STATE_FILE, "w");
  if (!f) {
    Serial.println("❌ Failed to open state file for writing");
    return;
  }
  if (serializeJson(doc, f) == 0) {
    Serial.println("❌ Failed to write to state file");
  } else {
    Serial.println("💾 State saved");
  }
  f.close();
}

void loadState() {
  if (!LittleFS.exists(STATE_FILE)) {
    Serial.println("ℹ No saved state, creating defaults");
    saveState();
    return;
  }

  File f = LittleFS.open(STATE_FILE, "r");
  if (!f) {
    Serial.println("❌ Failed to open state file for reading");
    return;
  }
  if (f.size() == 0) {
    Serial.println("ℹ State file empty");
    f.close();
    return;
  }

  DynamicJsonDocument doc(1024);
  auto err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("❌ Failed to parse state JSON");
    return;
  }

  daily_kwh      = doc["daily_kwh"]      | 0.0;
  yesterday_kwh  = doc["yesterday_kwh"]  | 0.0;
  month_kwh      = doc["month_kwh"]      | 0.0;
  last_month_kwh = doc["last_month_kwh"] | 0.0;
  tariffRpKwh    = doc["tariff"]         | tariffRpKwh;
  lastDayNum     = doc["lastDayNum"]     | -1;
  month_id       = doc["month_id"]       | -1;
  last_month_id  = doc["last_month_id"]  | -1;

  uint32_t secs = doc["timer_accum_sec"] | 0;
  timerAccumulatedMs = (unsigned long)secs * 1000UL;

  Serial.printf("✅ Loaded state: today=%.4f kWh, tariff=₹%.2f, timer_accum=%lus\n",
    daily_kwh, tariffRpKwh, (unsigned long)(timerAccumulatedMs/1000UL));
}

float power_smoothed(float p_now) {
  p_buf[p_idx] = isnan(p_now) ? 0.0f : p_now;
  p_idx = (p_idx + 1) % POWER_AVG_WINDOW;
  if (p_idx == 0) p_filled = true;

  float sum = 0;
  uint8_t n = p_filled ? POWER_AVG_WINDOW : p_idx;
  if (n == 0) return p_now;
  for (uint8_t i = 0; i < n; i++) sum += p_buf[i];
  return sum / n;
}

void handleDebug() {
  String message = "Debug Info:\n";
  message += "WiFi Status: " + String(WiFi.status()) + "\n";
  message += "IP Address: " + WiFi.localIP().toString() + "\n";
  message += "LittleFS State: " + String(LittleFS.exists(STATE_FILE) ? "File exists" : "File missing") + "\n";
  message += "PZEM Pointer: " + String((pzem != nullptr) ? "Valid" : "Null") + "\n";
  message += "Timer running: " + String(timerRunning) + ", accumSec=" + String(timerAccumulatedMs / 1000UL) + "\n";
  server.send(200, "text/plain", message);
}

/* ====== DASHBOARD HTML (UI designed) ====== */
String dashboardHtml() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>Smart IoT Energy Meter</title>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <link href="https://fonts.googleapis.com/css2?family=Poppins:wght@400;600;700&display=swap" rel="stylesheet">
  <style>
    :root { --bg1:#0f2027; --bg2:#203a43; --accent:#00e5ff; --muted:#9fbad7; --panel: rgba(255,255,255,0.06); }
    body { font-family:'Poppins',sans-serif; margin:0; padding:28px; background:linear-gradient(135deg,var(--bg1),var(--bg2)); color:#e8f5ff; text-align:center; }
    h1 { font-size:44px; margin:4px 0 18px; color:var(--accent); text-shadow:0 6px 30px rgba(0,229,255,0.16); }
    .grid { display:grid; gap:16px; grid-template-columns: repeat(auto-fit,minmax(240px,1fr)); align-items:start; margin-bottom:18px; }
    .card { background:var(--panel); border-radius:16px; padding:18px; box-shadow: 0 12px 30px rgba(0,0,0,0.45); backdrop-filter: blur(6px); text-align:left; }
    .card h3 { margin:0 0 8px; color:#90e9ff; font-size:13px; letter-spacing:1px; text-transform:uppercase; }
    .value { font-size:20px; font-weight:700; color:#ffffff; }
    .sub { margin-top:8px; font-size:12px; color:var(--muted); }
    .timer-card { grid-column: 1/-1; display:flex; align-items:center; justify-content:space-between; padding:24px; border: 2px solid rgba(0,229,255,0.12); box-shadow: 0 16px 40px rgba(0,229,255,0.06); }
    .timer-left { text-align:left; }
    .timer-title { font-size:14px; color:#9beaff; text-transform:uppercase; letter-spacing:1px; margin-bottom:8px; }
    .timer { font-size:44px; font-weight:800; color:#00ffd6; text-shadow: 0 6px 20px rgba(0,255,214,0.12); }
    .timer-status { margin-top:10px; font-size:13px; color:var(--muted); }
    .controls { margin-top:18px; display:flex; gap:10px; justify-content:center; flex-wrap:wrap; }
    input[type=number] { padding:10px 12px; border-radius:10px; border:none; background:rgba(255,255,255,0.06); color:#fff; width:160px; }
    button { padding:10px 16px; border-radius:12px; border:none; font-weight:700; cursor:pointer; transition: transform 0.12s ease; }
    button:hover { transform: translateY(-3px); }
    .btn-primary { background: linear-gradient(90deg,#00c6ff,#00ffd6); color:#012; }
    .btn-plain { background:#2f3b43; color:#fff; }
    .btn-danger { background: linear-gradient(90deg,#ff6b6b,#ff3b3b); color:#fff; }
    footer { margin-top:22px; color:var(--muted); font-size:13px; }
    @media (max-width:700px) { .timer { font-size:32px; } .timer-card{flex-direction:column; gap:14px} }
  </style>
</head>
<body>
  <h1>⚡ Smart IoT Energy Meter ⚡</h1>

  <div class="grid" id="cards">
    <!-- injected -->
  </div>

  <div class="controls">
    <input id="tariffInput" type="number" step="0.01" min="0" placeholder="Tariff ₹/kWh" />
    <button class="btn-primary" onclick="updateTariff()">Update Tariff</button>

    <button class="btn-primary" onclick="startTimer()">Start Timer</button>
    <button class="btn-plain" onclick="stopTimer()">Stop Timer</button>
    <button class="btn-danger" onclick="resetTimer()">Reset Timer</button>

    <button class="btn-plain" onclick="formatFS()">Format FS</button>
    <button class="btn-plain" onclick="resetEnergy()">Reset Energy</button>
    <button class="btn-plain" onclick="showDebug()">Debug</button>
  </div>

  <footer>Auto refresh every 2s • Device: PZEM004Tv30</footer>

<script>
function pad2(n){ return n<10?'0'+n:String(n); }
function formatHMS(sec){
  sec = Math.max(0, Math.floor(sec));
  const h = Math.floor(sec/3600);
  const m = Math.floor((sec%3600)/60);
  const s = sec%60;
  return pad2(h)+':'+pad2(m)+':'+pad2(s);
}
function card(title, val, sub='', cls=''){ 
  return `<div class="card ${cls}"><h3>${title}</h3><div class="value">${val}</div>${sub?('<div class="sub">'+sub+'</div>'):''}</div>`;
}

async function refresh(){
  try{
    const res = await fetch('/api/status');
    if(!res.ok) throw new Error('HTTP '+res.status);
    const j = await res.json();

    let html='';
    html += card('Voltage', j.voltage.toFixed(1)+' V', 'Line voltage');
    html += card('Current', j.current_display, 'Measured current');
    html += card('Power', j.power.toFixed(2)+' W', 'Smoothed');
    html += card('Per Minute', j.energy_per_min.toFixed(3)+' Wh (₹'+j.cost_per_min.toFixed(4)+')');
    html += card('Today Energy', j.today_kwh.toFixed(3)+' kWh (₹'+j.cost_today.toFixed(2)+')');
    html += card('Month Energy', j.month_kwh.toFixed(3)+' kWh (₹'+j.cost_month.toFixed(2)+')');
    html += card('Yesterday', j.yesterday_kwh.toFixed(3)+' kWh');
    html += card('Last Month', j.last_month_kwh.toFixed(3)+' kWh (₹'+j.last_month_cost.toFixed(2)+')');
    html += card('Tariff', '₹'+j.tariff+' /kWh');

    // timer card spanning full width
    const timerSec = j.timer_sec;
    const timerRunning = j.timer_running;
    const timerLabel = timerRunning ? '🟢 Running' : '🔴 Stopped';
    html += `<div class="card timer-card"><div class="timer-left"><div class="timer-title">Manual Timer</div><div class="timer">${formatHMS(timerSec)}</div><div class="timer-status">Status: ${timerLabel}</div></div></div>`;

    document.getElementById('cards').innerHTML = html;
  } catch(e){
    console.error(e);
    document.getElementById('cards').innerHTML = '<div class="card"><h3>Error</h3><div class="value">Failed to load data</div></div>';
  }
}

async function updateTariff(){
  const v = document.getElementById('tariffInput').value;
  if(!v || isNaN(v) || Number(v) <= 0){ alert('Enter valid tariff'); return; }
  await fetch('/settariff?tariff='+encodeURIComponent(v), { method:'POST' });
  refresh();
}
async function startTimer(){ await fetch('/starttimer', { method:'POST' }); refresh(); }
async function stopTimer(){ await fetch('/stoptimer', { method:'POST' }); refresh(); }
async function resetTimer(){ if(!confirm('Reset timer to 00:00:00?')) return; await fetch('/resettimer', { method:'POST' }); refresh(); }
async function formatFS(){ if(!confirm('Format LittleFS? This will delete saved state.')) return; const r = await fetch('/formatfs', { method:'POST' }); alert(r.ok?'Formatted':'Failed'); refresh(); }
async function resetEnergy(){ if(!confirm('Reset energy counters?')) return; await fetch('/resetenergy', { method:'POST' }); refresh(); }
async function showDebug(){ const r = await fetch('/debug'); const txt = await r.text(); alert(txt); }

setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>
)rawliteral";
}

/* ====== HTTP HANDLERS ====== */
void handleRoot() { server.send(200, "text/html", dashboardHtml()); }

void handleSetTariff() {
  if (server.hasArg("tariff")) {
    tariffRpKwh = server.arg("tariff").toFloat();
    saveState();
    Serial.printf("Tariff set to ₹%.2f\n", tariffRpKwh);
  }
  server.send(204);
}

void handleFormatFS() {
  if (LittleFS.format()) {
    Serial.println("LittleFS formatted successfully");
    saveState(); // new clean state
    server.send(200, "text/plain", "LittleFS formatted successfully");
  } else {
    Serial.println("Failed to format LittleFS");
    server.send(500, "text/plain", "Failed to format LittleFS");
  }
}

void handleResetEnergy() {
  daily_kwh = 0.0;
  yesterday_kwh = 0.0;
  month_kwh = 0.0;
  last_month_kwh = 0.0;
  saveState();
  Serial.println("Energy counters reset");
  server.send(204);
}

void handleStartTimer() {
  if (!timerRunning) {
    timerStartMs = millis();
    timerRunning = true;
    Serial.println("Timer started");
    saveState();
  }
  server.send(204);
}

void handleStopTimer() {
  if (timerRunning) {
    unsigned long now = millis();
    timerAccumulatedMs += (now - timerStartMs);
    timerRunning = false;
    Serial.printf("Timer stopped, accumSec=%lu\n", timerAccumulatedMs / 1000UL);
    saveState();
  }
  server.send(204);
}

void handleResetTimer() {
  timerRunning = false;
  timerAccumulatedMs = 0;
  timerStartMs = 0;
  saveState();
  Serial.println("Timer reset");
  server.send(204);
}

void handleApiStatus() {
  float v = 0, c = 0, p = 0;
  if (pzem != nullptr) {
    v = pzem->voltage();
    c = pzem->current();
    p = pzem->power();
  }

  // if mains voltage is very small/unavailable, treat all readings as 0 (prevents garbage)
  if (isnan(v) || v < 1.0) {
    v = 0;
    c = 0;
    p = 0;
  }

  // sanitize raw readings
  v = (float)safeNumber(v);
  c = (float)safeNumber(c);
  p = (float)safeNumber(p);

  float p_avg = power_smoothed(p);
  double energy_per_min_Wh = p_avg / 60.0;
  double cost_per_min = (energy_per_min_Wh / 1000.0) * tariffRpKwh;

  // timer calculation
  unsigned long timerNowMs = timerAccumulatedMs;
  if (timerRunning) timerNowMs += (millis() - timerStartMs);
  uint32_t timerSec = (uint32_t)(timerNowMs / 1000UL);

  DynamicJsonDocument doc(2048);
  doc["voltage"] = v;
  doc["power"] = p_avg;
  doc["standby_threshold"] = STANDBY_THRESHOLD_W;

  String currentStr;
  if (c < 1.0) currentStr = String(c * 1000.0, 1) + " mA";
  else currentStr = String(c, 2) + " A";
  doc["current_display"] = currentStr;
  doc["current"] = c;

  doc["today_kwh"] = safeNumber(daily_kwh);
  doc["yesterday_kwh"] = safeNumber(yesterday_kwh);
  doc["month_kwh"] = safeNumber(month_kwh);
  doc["last_month_kwh"] = safeNumber(last_month_kwh);

  doc["tariff"] = tariffRpKwh;
  doc["cost_today"] = safeNumber(daily_kwh * tariffRpKwh);
  doc["cost_month"] = safeNumber(month_kwh * tariffRpKwh);
  doc["last_month_cost"] = safeNumber(last_month_kwh * tariffRpKwh);

  doc["energy_per_min"] = safeNumber(energy_per_min_Wh);
  doc["cost_per_min"] = safeNumber(cost_per_min);

  doc["month_id"] = month_id;
  doc["last_month_id"] = last_month_id;
  doc["epoch"] = (uint32_t)timeClient.getEpochTime();

  // timer fields
  doc["timer_running"] = timerRunning;
  doc["timer_sec"] = timerSec;
  doc["timer_display"] = String((timerSec/3600)%100) + ":" + String((timerSec%3600)/60) + ":" + String(timerSec%60);

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

/* ====== SETUP ====== */
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("Starting Smart Energy Meter...");

  if (!LittleFS.begin(true)) {
    Serial.println("❌ LittleFS mount failed");
  } else {
    Serial.println("✅ LittleFS mounted successfully");
    File root = LittleFS.open("/");
    File file = root.openNextFile();
    Serial.println("Files in LittleFS:");
    while (file) {
      Serial.printf("  %s (%d bytes)\n", file.name(), file.size());
      file = root.openNextFile();
    }
  }

  loadState();

  // Initialize PZEM (Serial2)
  Serial2.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);
  pzem = new PZEM004Tv30(Serial2, PZEM_RX, PZEM_TX);
  if (pzem) Serial.println("✅ PZEM initialized");
  else Serial.println("❌ PZEM init failed");

  // WiFi
   WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000UL) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected");
    Serial.print("📶 IP Address: ");
    Serial.println(WiFi.localIP());   // <-- ADDED HERE
  } else {
    Serial.println("\n⚠️ WiFi failed, continuing in offline mode...");
  }


  // NTP
  timeClient.begin();
  timeClient.update();
  time_t ep = timeClient.getEpochTime();
  lastDayNum = (lastDayNum < 0) ? (int)(ep / 86400) : lastDayNum;
  if (month_id < 0) month_id = yyyymm_from_epoch(ep);

  // Routes
  server.on("/", handleRoot);
  server.on("/api/status", handleApiStatus);
  server.on("/settariff", HTTP_POST, handleSetTariff);
  server.on("/formatfs", HTTP_POST, handleFormatFS);
  server.on("/resetenergy", HTTP_POST, handleResetEnergy);
  server.on("/debug", HTTP_GET, [](void){ handleDebug(); });

  server.on("/starttimer", HTTP_POST, handleStartTimer);
  server.on("/stoptimer", HTTP_POST, handleStopTimer);
  server.on("/resettimer", HTTP_POST, handleResetTimer);

  server.begin();
  Serial.println("🌐 Web server started");

  prevSampleTimeMs = millis();
  lastSaveMs = millis();
}

/* ====== LOOP ====== */
void loop() {
  server.handleClient();
  unsigned long nowMs = millis();

  if (nowMs - prevSampleTimeMs >= SAMPLE_INTERVAL_MS) {
    static unsigned long lastMillisForDt = 0;
    if (lastMillisForDt == 0) lastMillisForDt = nowMs - SAMPLE_INTERVAL_MS;
    double dt = (nowMs - lastMillisForDt) / 1000.0; // seconds
    lastMillisForDt = nowMs;

    float v = 0.0f, c = 0.0f, p = 0.0f;
    if (pzem != nullptr) {
      v = pzem->voltage();
      c = pzem->current();
      p = pzem->power();
    }

    // if mains is off/invalid, treat as zero (prevents garbage math)
    if (isnan(v) || v < 1.0) { v = 0; c = 0; p = 0; }

    // sanitize & smooth power
    p = power_smoothed((float)safeNumber(p));

    // integrate energy (kWh)
    daily_kwh += (p * dt) / 3600.0;

    prevSampleTimeMs = nowMs;
  }

  if (nowMs - lastSaveMs >= SAVE_INTERVAL_MS) {
    saveState();
    lastSaveMs = nowMs;
  }

  static unsigned long lastTimeUpd = 0;
  if (millis() - lastTimeUpd > 60000UL) {
    timeClient.update();
    lastTimeUpd = millis();
  }

  time_t epoch = timeClient.getEpochTime();
  int curDay = epoch / 86400;
  int curMonthId = yyyymm_from_epoch(epoch);

  // daily rollover
  if (curDay != lastDayNum) {
    month_kwh += daily_kwh;
    yesterday_kwh = daily_kwh;
    daily_kwh = 0.0;
    lastDayNum = curDay;
    saveState();
    Serial.println("🌙 Midnight reset done");
  }

  // month rollover
  if (curMonthId != month_id) {
    last_month_kwh = month_kwh;
    last_month_id = month_id;
    month_id = curMonthId;
    month_kwh = 0.0;
    saveState();
    Serial.println("📅 Month rollover done");
  }

  delay(10);
}


