/*
  Sourdough Starter Monitor — v0.5
  ================================
  Baseline: pam's v0.4 sketch (archived at ../archive/sourdough_v0_4.ino).

  WHAT CHANGED IN v0.5 (feature 1: local web dashboard)
  -----------------------------------------------------
  + Secrets moved out of the sketch into `secrets.h` (gitignored).
  + 24-hour ring buffer of samples in RAM (1 sample/minute, ~17 KB).
  + HTTP server on port 80:
      GET  /              single-page dashboard (HTML/CSS/JS, no CDN)
      GET  /api/now       live readings as JSON
      GET  /api/history   ring buffer as compact JSON arrays (chunked)
      POST /api/calibrate same action as the physical button
  + mDNS: reachable at http://sourdough.local/ as well as by IP.
  + Optional NTP so the chart can show wall-clock times; falls back to
    "minutes ago" if NTP never syncs.
  + loop() restructured: the 2-second sampling gate no longer blocks
    handleClient()/checkButton(), which now run every iteration.

  UNCHANGED ON PURPOSE
  --------------------
  The state machine (IDLE/INITIAL/RISING/PEAKED/FALLING), its thresholds,
  the distance smoothing, and every Telegram message are byte-for-byte the
  v0.4 behaviour. Serial output format is also unchanged.

  Board: ESP32 DevKitC-32 (WROOM-32).  Wiring: I2C SDA=21 SCL=22,
  SHT31 @0x44, VL53L0X @0x29, button on GPIO19 (INPUT_PULLUP).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include "Adafruit_VL53L0X.h"

#include "secrets.h"   // copy secrets.h.example -> secrets.h and fill in

// ---------------------------------------------------------------- config --
const int BUTTON_PIN = 19;

const int16_t RISING_THRESHOLD_MM  = 3;
const int16_t PEAK_PLATEAU_MINUTES = 10;
const int16_t FALL_FROM_PEAK_MM    = 5;

const unsigned long SAMPLE_INTERVAL_MS = 2000;    // sensor read + serial line
const unsigned long LOG_INTERVAL_MS    = 60000;   // one history point / minute
const uint16_t      HISTORY_LEN        = 1440;    // 1440 min = 24 h

// ---------------------------------------------------------------- globals --
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_VL53L0X vl53 = Adafruit_VL53L0X();
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
WebServer server(80);

enum State { ST_IDLE, ST_INITIAL, ST_RISING, ST_PEAKED, ST_FALLING };
State state = ST_IDLE;
const char* stateNames[] = {"IDLE", "INITIAL", "RISING", "PEAKED", "FALLING"};

bool sht31_ok = false, vl53_ok = false, wifi_ok = false, ntp_ok = false;
uint16_t baseline_dist = 0;
unsigned long baseline_time = 0;
int16_t peak_rise_mm = 0;
unsigned long peak_time = 0;

const int SMOOTH_N = 5;
uint16_t dist_buffer[SMOOTH_N] = {0};
int dist_idx = 0;
bool buffer_full = false;

bool last_button = HIGH;
unsigned long last_button_change = 0;

// Latest readings, kept so the HTTP handlers never touch the I2C bus.
float    last_temp = NAN;
float    last_hum  = NAN;
uint16_t last_dist = 0;
int16_t  last_rise = 0;
unsigned long last_sample_ms = 0;

// ------------------------------------------------------------ ring buffer --
// 12 bytes/sample x 1440 = ~17 KB of the ESP32's ~320 KB DRAM.
struct Sample {
  uint32_t t;         // seconds since boot
  int16_t  dist;      // mm, 0 = out of range
  int16_t  rise;      // mm above baseline
  int16_t  temp_c10;  // temperature x10, INT16_MIN = no reading
  uint8_t  hum;       // %RH, 255 = no reading
  uint8_t  state;     // index into stateNames[]
};

Sample history[HISTORY_LEN];
uint16_t hist_count = 0;   // how many slots are populated (caps at HISTORY_LEN)
uint16_t hist_head  = 0;   // next slot to write
unsigned long last_log_ms = 0;

void historyPush(uint32_t t_s, uint16_t dist, int16_t rise,
                 float temp, float hum, uint8_t st) {
  Sample& s = history[hist_head];
  s.t        = t_s;
  s.dist     = (int16_t)dist;
  s.rise     = rise;
  s.temp_c10 = isnan(temp) ? INT16_MIN : (int16_t)lroundf(temp * 10.0f);
  s.hum      = isnan(hum)  ? 255       : (uint8_t)constrain(lroundf(hum), 0, 100);
  s.state    = st;
  hist_head  = (hist_head + 1) % HISTORY_LEN;
  if (hist_count < HISTORY_LEN) hist_count++;
}

// ------------------------------------------------------------- v0.4 logic --
void sendTelegram(const String& msg) {
  if (!wifi_ok) {
    Serial.println("[TG] WiFi down, skipping message");
    return;
  }
  Serial.print("[TG] Sending: ");
  Serial.println(msg);
  bool ok = bot.sendMessage(CHAT_ID, msg, "");
  Serial.println(ok ? "[TG] Sent" : "[TG] FAILED");
}

uint16_t readDistance() {
  if (!vl53_ok) return 0;
  VL53L0X_RangingMeasurementData_t m;
  vl53.rangingTest(&m, false);

  // Reject bad readings: RangeStatus 4 = out of range, or nonsense values
  if (m.RangeStatus != 0 || m.RangeMilliMeter > 2000 || m.RangeMilliMeter < 10) {
    return 0;
  }

  dist_buffer[dist_idx] = m.RangeMilliMeter;
  dist_idx = (dist_idx + 1) % SMOOTH_N;
  if (dist_idx == 0) buffer_full = true;

  int n = buffer_full ? SMOOTH_N : dist_idx;
  if (n == 0) return m.RangeMilliMeter;
  uint32_t sum = 0;
  for (int i = 0; i < n; i++) sum += dist_buffer[i];
  return sum / n;
}

void calibrate(uint16_t d) {
  if (d == 0) {
    Serial.println(">> Cannot calibrate: sensor out of range");
    sendTelegram("⚠️ Calibration failed — starter too far from sensor. Wait for it to rise, then try again.");
    return;
  }
  baseline_dist = d;
  baseline_time = millis();
  peak_rise_mm = 0;
  peak_time = baseline_time;
  state = ST_INITIAL;
  for (int i = 0; i < SMOOTH_N; i++) dist_buffer[i] = d;
  buffer_full = true;
  Serial.print(">> CALIBRATED at ");
  Serial.print(d);
  Serial.println(" mm");

  float temp = sht31_ok ? sht31.readTemperature() : NAN;
  String msg = "🌱 Starter calibrated\n";
  msg += "Baseline: " + String(d) + " mm";
  if (!isnan(temp)) msg += "\nTemp: " + String(temp, 1) + "°C";
  sendTelegram(msg);
}

void checkButton() {
  bool now_b = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  if (last_button == HIGH && now_b == LOW && (now - last_button_change) > 200) {
    last_button_change = now;
    calibrate(readDistance());
  }
  last_button = now_b;
}

void updateState(int16_t rise_mm) {
  unsigned long mins_since_peak = (millis() - peak_time) / 60000;
  unsigned long mins_elapsed = (millis() - baseline_time) / 60000;

  switch (state) {
    case ST_INITIAL:
      if (rise_mm >= RISING_THRESHOLD_MM) {
        state = ST_RISING;
        Serial.println(">> State: RISING");
        sendTelegram("📈 Starter is rising\n"
                     "Gained: " + String(rise_mm) + " mm\n"
                     "Elapsed: " + String(mins_elapsed) + " min");
      }
      break;
    case ST_RISING:
      if (mins_since_peak >= PEAK_PLATEAU_MINUTES && peak_rise_mm > RISING_THRESHOLD_MM) {
        state = ST_PEAKED;
        Serial.println(">> State: PEAKED");
        sendTelegram("🎯 Starter has peaked!\n"
                     "Peak rise: " + String(peak_rise_mm) + " mm\n"
                     "Time to peak: " + String(mins_elapsed) + " min\n"
                     "Ready to use or feed.");
      }
      break;
    case ST_PEAKED:
      if (peak_rise_mm - rise_mm >= FALL_FROM_PEAK_MM) {
        state = ST_FALLING;
        Serial.println(">> State: FALLING");
        sendTelegram("📉 Past peak, falling\n"
                     "Down " + String(peak_rise_mm - rise_mm) + " mm from peak.\n"
                     "Feed soon.");
      }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------- dashboard ---
// Single page, no external requests: it must work when the internet is out.
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sourdough Monitor</title>
<style>
:root{--bg:#14110e;--card:#1f1b16;--line:#332c24;--ink:#f3ece1;--dim:#a2937f;
      --rise:#e8a33d;--temp:#5fb0d6;--ok:#7ec86a;--warn:#e0603c}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
     font:15px/1.45 ui-sans-serif,system-ui,-apple-system,"Segoe UI",sans-serif}
header{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;
       padding:18px 20px 10px}
h1{font-size:18px;margin:0;font-weight:600}
#state{font-size:12px;letter-spacing:.08em;text-transform:uppercase;
       padding:3px 9px;border-radius:99px;background:var(--line);color:var(--dim)}
#state.live{background:#2c3a24;color:var(--ok)}
#age{margin-left:auto;font-size:12px;color:var(--dim)}
main{padding:0 20px 24px;max-width:900px}
.grid{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(130px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px 14px}
.card .k{font-size:11px;letter-spacing:.07em;text-transform:uppercase;color:var(--dim)}
.card .v{font-size:26px;font-weight:600;margin-top:2px;font-variant-numeric:tabular-nums}
.card .u{font-size:13px;font-weight:400;color:var(--dim);margin-left:3px}
.chartwrap{background:var(--card);border:1px solid var(--line);border-radius:10px;
           padding:12px;margin-top:12px}
canvas{width:100%;height:260px;display:block}
.legend{display:flex;gap:16px;font-size:12px;color:var(--dim);margin:6px 2px 0}
.sw{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:5px;
    vertical-align:middle}
.row{display:flex;gap:8px;align-items:center;margin-top:12px;flex-wrap:wrap}
button{background:var(--line);color:var(--ink);border:1px solid #453b30;
       border-radius:8px;padding:9px 14px;font-size:14px;cursor:pointer}
button:hover{background:#3d342a}
button.primary{background:var(--rise);border-color:var(--rise);color:#1a1409;font-weight:600}
#msg{font-size:13px;color:var(--dim)}
footer{color:var(--dim);font-size:12px;padding:0 20px 20px;max-width:900px}
.bad{color:var(--warn)}
</style></head><body>

<header>
  <h1>Sourdough Monitor</h1>
  <span id="state">--</span>
  <span id="age"></span>
</header>

<main>
  <div class="grid">
    <div class="card"><div class="k">Rise</div>
      <div class="v"><span id="rise">--</span><span class="u">mm</span></div></div>
    <div class="card"><div class="k">Rise</div>
      <div class="v"><span id="risepct">--</span><span class="u">%</span></div></div>
    <div class="card"><div class="k">Peak so far</div>
      <div class="v"><span id="peak">--</span><span class="u">mm</span></div></div>
    <div class="card"><div class="k">Elapsed</div>
      <div class="v"><span id="elapsed">--</span></div></div>
    <div class="card"><div class="k">Temperature</div>
      <div class="v"><span id="temp">--</span><span class="u">&deg;C</span></div></div>
    <div class="card"><div class="k">Humidity</div>
      <div class="v"><span id="hum">--</span><span class="u">%</span></div></div>
    <div class="card"><div class="k">Distance</div>
      <div class="v"><span id="dist">--</span><span class="u">mm</span></div></div>
    <div class="card"><div class="k">Baseline</div>
      <div class="v"><span id="base">--</span><span class="u">mm</span></div></div>
  </div>

  <div class="chartwrap">
    <canvas id="chart"></canvas>
    <div class="legend">
      <span><i class="sw" style="background:var(--rise)"></i>Rise (mm)</span>
      <span><i class="sw" style="background:var(--temp)"></i>Temp (&deg;C)</span>
      <span id="span"></span>
    </div>
  </div>

  <div class="row">
    <button class="primary" id="cal">Set baseline (just fed)</button>
    <button id="csv">Download CSV</button>
    <span id="msg"></span>
  </div>
</main>

<footer id="foot"></footer>

<script>
const $=id=>document.getElementById(id);
let hist={s:[],t0:0}, now=null;

function fmtDur(m){ if(m==null)return'--';
  const h=Math.floor(m/60), mm=m%60; return h? h+'h '+mm+'m' : mm+'m'; }

async function tick(){
  try{
    const r=await fetch('/api/now',{cache:'no-store'});
    now=await r.json();
    $('state').textContent=now.state;
    $('state').className=now.state==='IDLE'?'':'live';
    $('rise').textContent = now.dist? now.rise : '--';
    $('risepct').textContent = now.rise_pct==null? '--' : now.rise_pct.toFixed(0);
    $('peak').textContent = now.peak_rise;
    $('elapsed').textContent = now.state==='IDLE'? '--' : fmtDur(now.elapsed_min);
    $('temp').textContent = now.temp==null? '--' : now.temp.toFixed(1);
    $('hum').textContent  = now.hum==null? '--' : now.hum.toFixed(0);
    $('dist').textContent = now.dist? now.dist : 'out of range';
    $('base').textContent = now.baseline? now.baseline : '--';
    $('age').textContent  = 'updated '+new Date().toLocaleTimeString();
    $('foot').innerHTML = 'ESP32 up '+fmtDur(Math.floor(now.uptime_s/60))
      +' &middot; SHT31 '+(now.sht31?'ok':'<span class=bad>missing</span>')
      +' &middot; VL53L0X '+(now.vl53?'ok':'<span class=bad>missing</span>')
      +' &middot; free heap '+(now.heap/1024).toFixed(0)+' KB'
      +' &middot; '+now.history_n+' history points';
  }catch(e){ $('age').textContent='connection lost'; }
}

async function loadHistory(){
  try{
    const r=await fetch('/api/history',{cache:'no-store'});
    hist=await r.json(); draw();
  }catch(e){}
}

function draw(){
  const c=$('chart'), dpr=window.devicePixelRatio||1;
  const w=c.clientWidth, h=c.clientHeight;
  c.width=w*dpr; c.height=h*dpr;
  const x=c.getContext('2d'); x.scale(dpr,dpr);
  x.clearRect(0,0,w,h);
  const L=42,R=42,T=12,B=24, pw=w-L-R, ph=h-T-B;
  const s=hist.s||[];
  if(s.length<2){ x.fillStyle='#a2937f'; x.font='13px sans-serif';
    x.fillText('Collecting data — one point per minute.',L,T+ph/2); return; }

  const t=s.map(p=>p[0]), rise=s.map(p=>p[2]),
        tp=s.map(p=>p[3]===-32768?null:p[3]/10);
  const t0=t[0], t1=t[t.length-1];
  const rmin=Math.min(0,...rise), rmax=Math.max(10,...rise);
  const tv=tp.filter(v=>v!=null);
  const tmin=tv.length?Math.min(...tv)-1:18, tmax=tv.length?Math.max(...tv)+1:28;
  const X=v=>L+(v-t0)/Math.max(1,t1-t0)*pw;
  const Yr=v=>T+ph-(v-rmin)/Math.max(1,rmax-rmin)*ph;
  const Yt=v=>T+ph-(v-tmin)/Math.max(0.1,tmax-tmin)*ph;

  // grid + left axis (rise mm)
  x.strokeStyle='#332c24'; x.fillStyle='#a2937f'; x.font='11px sans-serif'; x.lineWidth=1;
  for(let i=0;i<=4;i++){
    const v=rmin+(rmax-rmin)*i/4, y=Yr(v);
    x.beginPath(); x.moveTo(L,y); x.lineTo(L+pw,y); x.stroke();
    x.textAlign='right'; x.fillText(v.toFixed(0),L-6,y+4);
    const tvv=tmin+(tmax-tmin)*i/4;
    x.textAlign='left'; x.fillStyle='#5fb0d6';
    x.fillText(tvv.toFixed(0)+'\u00b0',L+pw+6,y+4); x.fillStyle='#a2937f';
  }
  // x labels
  x.textAlign='center';
  for(let i=0;i<=4;i++){
    const tt=t0+(t1-t0)*i/4;
    const mins=Math.round((t1-tt)/60);
    let lab = hist.epoch ? new Date((hist.epoch+(tt-hist.now))*1000)
                             .toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'})
                         : (mins===0?'now':'-'+fmtDur(mins));
    x.fillText(lab,X(tt),T+ph+16);
  }
  // temp line
  x.strokeStyle='#5fb0d6'; x.lineWidth=1.5; x.beginPath(); let pen=false;
  tp.forEach((v,i)=>{ if(v==null){pen=false;return;}
    const px=X(t[i]),py=Yt(v); pen?x.lineTo(px,py):x.moveTo(px,py); pen=true; });
  x.stroke();
  // rise line + fill
  x.strokeStyle='#e8a33d'; x.lineWidth=2; x.beginPath();
  rise.forEach((v,i)=>{ const px=X(t[i]),py=Yr(v); i?x.lineTo(px,py):x.moveTo(px,py); });
  x.stroke();
  x.lineTo(X(t1),Yr(rmin)); x.lineTo(X(t0),Yr(rmin)); x.closePath();
  x.fillStyle='rgba(232,163,61,.13)'; x.fill();

  $('span').textContent='window: '+fmtDur(Math.round((t1-t0)/60));
}

$('cal').onclick=async()=>{
  $('msg').textContent='setting baseline…';
  try{ const r=await fetch('/api/calibrate',{method:'POST'});
       const j=await r.json();
       $('msg').textContent = j.ok ? 'baseline set at '+j.baseline+' mm'
                                   : 'failed: '+j.error;
  }catch(e){ $('msg').textContent='failed: no response'; }
  tick();
};

$('csv').onclick=()=>{
  const rows=[['seconds_since_boot','distance_mm','rise_mm','temp_c','humidity_pct','state']];
  (hist.s||[]).forEach(p=>rows.push([p[0],p[1],p[2],
      p[3]===-32768?'':(p[3]/10).toFixed(1), p[4]===255?'':p[4], p[5]]));
  const blob=new Blob([rows.map(r=>r.join(',')).join('\n')],{type:'text/csv'});
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob); a.download='sourdough.csv'; a.click();
};

tick(); loadHistory();
setInterval(tick,2000);
setInterval(loadHistory,60000);
addEventListener('resize',draw);
</script></body></html>)HTML";

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleNow() {
  String j = "{";
  j += "\"state\":\"" + String(stateNames[state]) + "\"";
  j += ",\"dist\":" + String(last_dist);
  j += ",\"rise\":" + String(last_rise);
  j += ",\"peak_rise\":" + String(peak_rise_mm);
  j += ",\"baseline\":" + String(baseline_dist);
  if (state != ST_IDLE && baseline_dist > 0 && last_dist > 0) {
    // % rise relative to the starter column height under the sensor.
    float pct = 100.0f * (float)last_rise / (float)baseline_dist;
    j += ",\"rise_pct\":" + String(pct, 1);
    j += ",\"elapsed_min\":" + String((millis() - baseline_time) / 60000);
  } else {
    j += ",\"rise_pct\":null,\"elapsed_min\":null";
  }
  if (isnan(last_temp)) j += ",\"temp\":null"; else j += ",\"temp\":" + String(last_temp, 2);
  if (isnan(last_hum))  j += ",\"hum\":null";  else j += ",\"hum\":"  + String(last_hum, 1);
  j += ",\"uptime_s\":" + String(millis() / 1000);
  j += ",\"heap\":" + String(ESP.getFreeHeap());
  j += ",\"sht31\":" + String(sht31_ok ? "true" : "false");
  j += ",\"vl53\":" + String(vl53_ok ? "true" : "false");
  j += ",\"history_n\":" + String(hist_count);
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// Chunked so we never build a 60 KB String in RAM.
void handleHistory() {
  uint16_t n = hist_count;
  uint16_t start = (hist_count == HISTORY_LEN) ? hist_head : 0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", "");

  String head = "{\"n\":" + String(n) + ",\"now\":" + String(millis() / 1000);
  if (ntp_ok) head += ",\"epoch\":" + String((uint32_t)time(nullptr));
  head += ",\"s\":[";
  server.sendContent(head);

  String chunk;
  chunk.reserve(1024);
  for (uint16_t i = 0; i < n; i++) {
    const Sample& s = history[(start + i) % HISTORY_LEN];
    chunk += (i ? ",[" : "[");
    chunk += String(s.t);        chunk += ',';
    chunk += String(s.dist);     chunk += ',';
    chunk += String(s.rise);     chunk += ',';
    chunk += String(s.temp_c10); chunk += ',';
    chunk += String(s.hum);      chunk += ',';
    chunk += String(s.state);    chunk += ']';
    if (chunk.length() > 900) { server.sendContent(chunk); chunk = ""; }
  }
  chunk += "]}";
  server.sendContent(chunk);
  server.sendContent("");
}

void handleCalibrate() {
  uint16_t d = readDistance();
  if (d == 0) {
    server.send(200, "application/json",
                "{\"ok\":false,\"error\":\"sensor out of range\"}");
    return;   // deliberately no calibrate() call: keeps the Telegram
              // "calibration failed" alert tied to the physical button only
  }
  calibrate(d);
  server.send(200, "application/json",
              "{\"ok\":true,\"baseline\":" + String(d) + "}");
}

void handleNotFound() { server.send(404, "text/plain", "not found"); }

// ------------------------------------------------------------------ setup --
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Sourdough Sensor v0.5 ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin();

  if (sht31.begin(0x44)) { sht31_ok = true; Serial.println("SHT31: OK"); }
  else Serial.println("SHT31: NOT FOUND");

  if (vl53.begin()) { vl53_ok = true; Serial.println("VL53L0X: OK"); }
  else Serial.println("VL53L0X: NOT FOUND");

  // WiFi
  Serial.print("WiFi connecting to ");
  Serial.print(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // keeps the web server responsive
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_ok = true;
    Serial.print("\nWiFi: OK, IP=");
    Serial.println(WiFi.localIP());
    secured_client.setInsecure();  // skip cert validation (simpler; fine for a bot)

    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      Serial.print("mDNS: http://"); Serial.print(HOSTNAME); Serial.println(".local/");
    } else {
      Serial.println("mDNS: failed (use the IP)");
    }

    // Wall-clock time for chart labels. Non-fatal if it never syncs.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 10 && time(nullptr) < 1700000000; i++) delay(200);
    ntp_ok = time(nullptr) > 1700000000;
    Serial.println(ntp_ok ? "NTP: OK" : "NTP: not synced (chart uses relative time)");

    sendTelegram("🔌 Sourdough monitor online\n"
                 "IP: " + WiFi.localIP().toString() + "\n"
                 "Dashboard: http://" + WiFi.localIP().toString() + "/\n"
                 "Press the button after feeding to calibrate.");
  } else {
    Serial.println("\nWiFi: FAILED (continuing without Telegram)");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/now", HTTP_GET, handleNow);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server: started on port 80");

  Serial.println("\nt(s)\telapsed\tstate\tT\tH\tdist\trise\tpeak");
}

// ------------------------------------------------------------------- loop --
void loop() {
  // These must run every iteration — the sampling gate below used to
  // `return` early, which would have starved the web server.
  checkButton();
  server.handleClient();

  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();

  float temp = sht31_ok ? sht31.readTemperature() : NAN;
  float hum  = sht31_ok ? sht31.readHumidity()    : NAN;
  uint16_t dist = readDistance();

  int16_t rise_mm = 0;
  if (state != ST_IDLE && dist > 0) {
    rise_mm = (int16_t)baseline_dist - (int16_t)dist;
    if (rise_mm > peak_rise_mm) {
      peak_rise_mm = rise_mm;
      peak_time = millis();
    }
    updateState(rise_mm);
  }

  last_temp = temp; last_hum = hum; last_dist = dist; last_rise = rise_mm;

  if (millis() - last_log_ms >= LOG_INTERVAL_MS || hist_count == 0) {
    last_log_ms = millis();
    historyPush(millis() / 1000, dist, rise_mm, temp, hum, (uint8_t)state);
  }

  unsigned long elapsed_min = (state != ST_IDLE) ? (millis() - baseline_time) / 60000 : 0;

  Serial.print(millis() / 1000);   Serial.print("\t");
  Serial.print(elapsed_min);       Serial.print("m\t");
  Serial.print(stateNames[state]); Serial.print("\t");
  if (!isnan(temp)) Serial.print(temp, 1); else Serial.print("--");
  Serial.print("\t");
  if (!isnan(hum)) Serial.print(hum, 0); else Serial.print("--");
  Serial.print("\t");
  if (dist > 0) Serial.print(dist); else Serial.print("out");
  Serial.print("\t");
  Serial.print(rise_mm); Serial.print("\t");
  Serial.print(peak_rise_mm);
  Serial.println();
}
