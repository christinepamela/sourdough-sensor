/*
  Sourdough Starter Monitor — v0.9
  ================================
  Baseline: pam's v0.4 (archived). v0.5 added the web dashboard.

  WHAT CHANGED IN v0.6
  --------------------
  A. TRUE RISE % FROM JAR GEOMETRY
     + JAR_INTERIOR_HEIGHT_MM config (default 140), overridable at runtime
       and stored in NVS so measuring it doesn't mean reflashing.
     + starter_height_mm = JAR_INTERIOR_HEIGHT_MM - baseline_dist
     + rise_pct = 100 * rise_mm / starter_height_mm   -> 100% == doubled

  B. FEATURE 5 — PERSISTENCE ACROSS REBOOTS (ESP32 Preferences / NVS)
     + In-progress cycle survives a power cut: baseline, peak, state and
       elapsed time are restored and tracking continues.
     + Last 10 completed cycles stored as a ring.
     + GET /api/history_cycles returns them as JSON.
     + POST /api/config?jar=<mm> updates jar height (persisted).
     + POST /api/reset clears the in-progress cycle back to IDLE.

  WHAT CHANGED IN v0.7 — FEATURE 3, PREDICTION & PHASE
  ----------------------------------------------------
  + Cycles now also record baseline temperature and peak-hold duration.
  + "Comparable" cycles = temp-stable (stayed within +/-3 C of that cycle's
    own baseline temp) AND within +/-3 C of the current temperature.
  + With 3+ comparable cycles, predicts time-to-peak (median) and fires a
    single Telegram alert 30 min before the predicted peak.
  + Phase classification per cycle and for the starter overall:
    silent / bacterial / yeast-establishing / mature.
  + One-shot "mature starter" alert when 3 comparable cycles all doubled.
  + GET /api/prediction exposes the whole inference for inspection.

  All thresholds live in the PREDICTION TUNING block and are first-guess
  heuristics — they need real cycle data before they mean much.

  WHAT CHANGED IN v0.8 - FEATURE 2, OVERFLOW SAFETY
  -------------------------------------------------
  + Headroom alert when the starter comes within OVERFLOW_DIST_MM of the lid.
  + Runaway alert on a 5-minute least-squares rise rate (not endpoint
    differencing - the ToF is noisy and one bad reading would fake a spike).
  + Both use hysteresis rather than one-shot latching.

  WHAT CHANGED IN v0.9 - FEATURE 4, STARTER / LEVAIN MODES
  --------------------------------------------------------
  + Mode toggle, persisted in NVS. Changing mode resets tracking to IDLE and
    requires re-calibration (pam's spec).
  + Levain mode fires one target-hit alert per calibration and then KEEPS
    tracking, so the peak alert still fires afterwards.
  + Levain target is a persisted percentage, editable only in levain mode,
    drawn on the chart as a dashed line.
  + Levain cycles are never written to cycle history: different flour,
    hydration and quantity would poison the prediction model.
  + POST /api/reset_cycles wipes cycle history, the maturity latch and the
    live prediction, for starting clean on real hardware data.

  UNCHANGED ON PURPOSE
  --------------------
  The state machine (IDLE/INITIAL/RISING/PEAKED/FALLING), its thresholds,
  the distance smoothing, and the five original Telegram messages are the
  v0.4 behaviour verbatim. Serial output format is unchanged.

  Board: ESP32 DevKitC-32 (WROOM-32).  I2C SDA=21 SCL=22,
  SHT31 @0x44, VL53L0X @0x29, button GPIO19 (INPUT_PULLUP).

  BUILD: Partition Scheme = "Huge APP (3MB No OTA/1MB SPIFFS)".  Required,
  not optional — see README.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include "Adafruit_VL53L0X.h"

#include "sourdough_types.h"  // Sample, Cycle, LiveState, Phase — see header
                              // comment for why these can't live in the .ino
#include "secrets.h"   // copy secrets.h.example -> secrets.h and fill in

// ---------------------------------------------------------------- config --
const int BUTTON_PIN = 19;

// Measure this from the SENSOR FACE (lid underside, lid closed) to the
// INSIDE BOTTOM of the jar. That is the distance the sensor would read with
// an empty jar, and it is what makes rise % mean "doubled".
const uint16_t JAR_INTERIOR_HEIGHT_MM_DEFAULT = 140;

const int16_t RISING_THRESHOLD_MM  = 3;
const int16_t PEAK_PLATEAU_MINUTES = 10;
const int16_t FALL_FROM_PEAK_MM    = 5;

const unsigned long SAMPLE_INTERVAL_MS = 2000;    // sensor read + serial line
const unsigned long LOG_INTERVAL_MS    = 60000;   // one history point / minute
const uint16_t      HISTORY_LEN        = 1440;    // 1440 min = 24 h

// NVS wear control. State transitions always save; otherwise at most this
// often while a cycle is running. ~144 writes/day worst case.
const unsigned long STATE_SAVE_INTERVAL_MS = 600000;   // 10 min

// If the device was off longer than this, the rise curve has a real hole in
// it and the peak may have been missed entirely. Resume, but say so.
const uint32_t STALE_RESUME_MINUTES = 30;

const uint8_t  CYCLE_SLOTS  = 10;
const uint8_t  NVS_SCHEMA   = 2;   // bumped in v0.7: Cycle gained two fields

// -------------------------------------------------- OVERFLOW / SAFETY (F2) --
// pam's spec: headroom-based, not a fixed rise. Alerting on "50 mm risen"
// depends on where the baseline sat; alerting on "2 cm from the lid" is the
// thing that actually predicts a blowout.
// ------------------------------------------------------ MODES (FEATURE 4) --
// Levain mode watches for a user-set target % rise instead of the full cycle.
const uint16_t LEVAIN_TARGET_PCT_DEFAULT = 100;  // pam: default 100% (doubled)
const uint16_t LEVAIN_TARGET_PCT_MIN     = 10;
const uint16_t LEVAIN_TARGET_PCT_MAX     = 400;

const uint16_t OVERFLOW_DIST_MM   = 20;   // starter this close to the lid
const uint16_t OVERFLOW_REARM_MM  = 30;   // must fall back past this to re-arm
const int16_t  RUNAWAY_RATE_X100  = 300;  // 3.00 mm/min over a 5-min window
const uint16_t RUNAWAY_REARM_X100 = 150;  // re-arm below 1.50 mm/min
const unsigned long RATE_WINDOW_MS = 300000UL;  // 5 min
const uint8_t  RATE_SLOTS         = 32;   // ~10 s apart covers the window

// ------------------------------------------------------ PREDICTION TUNING --
// Every number here is a first-guess heuristic. None of it is validated
// against a real starter yet — pam's rig has a dead ToF sensor as of
// 2026-08-09. Expect to move these once actual cycle data exists.
const int16_t  TEMP_STABLE_BAND_C10   = 30;   // +/-3.0 C within a cycle
const int16_t  TEMP_MATCH_BAND_C10    = 30;   // +/-3.0 C cycle vs. now
const uint8_t  MIN_CYCLES_FOR_PREDICT = 3;    // pam: "3+ similar-temp cycles"
const uint16_t PREDICT_WARN_MIN       = 30;   // alert this far before peak
const uint16_t DOUBLED_PCT            = 100;  // 100% rise == doubled

// Phase classification thresholds (per cycle):
//   silent              — never peaked, or barely moved
//   bacterial           — fast to peak AND collapses quickly (short hold)
//   yeast-establishing  — rises, but slowly or not to a double
//   mature              — doubles, on a repeatable schedule
const int16_t  SILENT_RISE_PCT        = 20;   // under this = no real activity
const uint16_t BACTERIAL_PEAK_MIN     = 240;  // <=4 h to peak is fast
const uint16_t BACTERIAL_HOLD_MIN     = 45;   // ...and drops within 45 min
const uint16_t MATURE_MIN_PCT         = 100;  // must actually double

// ---------------------------------------------------------------- globals --
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_VL53L0X vl53 = Adafruit_VL53L0X();
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
WebServer server(80);
Preferences prefs;

enum State { ST_IDLE, ST_INITIAL, ST_RISING, ST_PEAKED, ST_FALLING };
State state = ST_IDLE;
const char* stateNames[] = {"IDLE", "INITIAL", "RISING", "PEAKED", "FALLING"};

bool sht31_ok = false, vl53_ok = false, wifi_ok = false, ntp_ok = false;
uint16_t baseline_dist = 0;
unsigned long baseline_time = 0;
int16_t peak_rise_mm = 0;
unsigned long peak_time = 0;

uint16_t jar_height_mm = JAR_INTERIOR_HEIGHT_MM_DEFAULT;

// Cycle accumulators (reset at each calibration)
uint32_t baseline_epoch = 0;      // wall-clock feed time, 0 if NTP was down
float    temp_sum = 0;
uint32_t temp_n = 0;
int16_t  temp_min_c10 = INT16_MAX;
int16_t  temp_max_c10 = INT16_MIN;
int16_t  baseline_temp_c10 = INT16_MIN;  // temp at feed time
unsigned long peaked_time = 0;    // when PEAKED was entered, 0 if not yet
bool     cycle_recorded = false;  // has this cycle been written to history?

// Prediction state (feature 3). Both alerts are one-shot per cycle/starter.
bool     prepeak_alert_sent = false;
uint16_t predicted_peak_min = 0;  // 0 = no prediction available
uint8_t  predict_n = 0;           // comparable cycles behind the prediction

// Mode state (feature 4). Mode and target persist across reboots.
Mode     mode = MODE_STARTER;
uint16_t levain_target_pct = LEVAIN_TARGET_PCT_DEFAULT;
bool     levain_target_hit = false;   // one-shot per cycle
uint16_t levain_hit_min = 0;          // minutes to target, for the summary

// Overflow / runaway state (feature 2). Latched with hysteresis rather than
// one-shot: a real blowout risk that persists deserves a second warning, but
// only after the starter has genuinely backed off and climbed again.
bool     overflow_alerted = false;
bool     runaway_alerted  = false;
int16_t  rise_rate_x100   = 0;    // current 5-min slope, mm/min x100

// Small ring of (time, rise) for the 5-minute slope. Sampled every 10 s so a
// single noisy ToF reading can't swing the rate on its own.
struct RatePoint { unsigned long t; int16_t rise; };
RatePoint rate_buf[RATE_SLOTS];
uint8_t rate_n = 0, rate_head = 0;
unsigned long last_rate_ms = 0;

const int SMOOTH_N = 5;
uint16_t dist_buffer[SMOOTH_N] = {0};
int dist_idx = 0;
bool buffer_full = false;

bool last_button = HIGH;
unsigned long last_button_change = 0;

// Latest readings, kept so HTTP handlers never touch the I2C bus.
float    last_temp = NAN;
float    last_hum  = NAN;
uint16_t last_dist = 0;
int16_t  last_rise = 0;
unsigned long last_sample_ms = 0;
unsigned long last_state_save_ms = 0;

// ------------------------------------------------------------ ring buffer --
// Sample is defined in sourdough_types.h.
// 12 bytes/sample x 1440 = ~17 KB of the ESP32's ~320 KB DRAM.
Sample history[HISTORY_LEN];
uint16_t hist_count = 0;
uint16_t hist_head  = 0;
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

// ------------------------------------------------- persisted cycle history --
// pam's field list, plus temp_min/temp_max: the "+/-3 C of baseline" rule for
// feature 3 cannot be evaluated from an average alone, and storing the spread
// (rather than a precomputed bool) means the threshold can change later
// without invalidating cycles already on disk.
// Cycle is defined in sourdough_types.h.

// Peak rise as a percentage of that cycle's own starter column.
int16_t cyclePct(const Cycle& c) {
  int16_t sh = (int16_t)c.jar_height_mm - (int16_t)c.baseline_mm;
  if (sh <= 0) return -1;
  return (int16_t)(100L * c.peak_rise_mm / sh);
}

// pam's rule: only trust cycles whose temperature stayed within +/-3 C of
// baseline. Read against the cycle's own baseline temp, so a jar that drifted
// warm all day is excluded even though its mean looks fine.
bool cycleTempStable(const Cycle& c) {
  if (c.baseline_temp_c10 == INT16_MIN ||
      c.temp_min_c10 == INT16_MIN || c.temp_max_c10 == INT16_MIN) return false;
  return (c.temp_max_c10 - c.baseline_temp_c10) <=  TEMP_STABLE_BAND_C10 &&
         (c.baseline_temp_c10 - c.temp_min_c10) <=  TEMP_STABLE_BAND_C10;
}

// Phase is defined in sourdough_types.h.
const char* phaseNames[] = {"unknown", "silent", "bacterial",
                            "yeast-establishing", "mature"};

// Per-cycle phase. "mature" here means "this cycle looked mature"; the
// starter-level verdict additionally requires repeatability across cycles.
Phase classifyCycle(const Cycle& c) {
  int16_t pct = cyclePct(c);
  if (pct < 0) return PH_UNKNOWN;
  if (c.time_to_peak_min == 0 || pct < SILENT_RISE_PCT) return PH_SILENT;

  // Fast peak that collapses straight away is the classic early-stage
  // bacterial (Leuconostoc) bloom, not yeast. Requires an observed hold:
  // peak_hold_min == 0 means it never left PEAKED, so we can't say.
  if (c.time_to_peak_min <= BACTERIAL_PEAK_MIN &&
      c.peak_hold_min > 0 && c.peak_hold_min <= BACTERIAL_HOLD_MIN) {
    return PH_BACTERIAL;
  }
  if (pct >= MATURE_MIN_PCT) return PH_MATURE;
  return PH_YEAST;
}

Cycle   cycles[CYCLE_SLOTS];
uint8_t cyc_count = 0;
uint8_t cyc_head  = 0;

// LiveState is defined in sourdough_types.h.

void saveCycles() {
  prefs.putBytes("cyc_blob", cycles, sizeof(cycles));
  prefs.putUChar("cyc_n", cyc_count);
  prefs.putUChar("cyc_head", cyc_head);
}

void loadCycles() {
  size_t got = prefs.getBytes("cyc_blob", cycles, sizeof(cycles));
  if (got != sizeof(cycles)) {          // absent or wrong size: start clean
    memset(cycles, 0, sizeof(cycles));
    cyc_count = 0; cyc_head = 0;
    return;
  }
  cyc_count = prefs.getUChar("cyc_n", 0);
  cyc_head  = prefs.getUChar("cyc_head", 0);
  if (cyc_count > CYCLE_SLOTS) cyc_count = CYCLE_SLOTS;
  if (cyc_head  >= CYCLE_SLOTS) cyc_head  = 0;
}

void saveLiveState() {
  LiveState ls;
  ls.state           = (uint8_t)state;
  ls.baseline_dist   = baseline_dist;
  ls.baseline_epoch  = baseline_epoch;
  ls.elapsed_min     = (state == ST_IDLE) ? 0 : (millis() - baseline_time) / 60000;
  ls.peak_rise_mm    = peak_rise_mm;
  ls.mins_since_peak = (state == ST_IDLE) ? 0 : (millis() - peak_time) / 60000;
  ls.peak_epoch      = (ntp_ok && baseline_epoch)
                       ? baseline_epoch + (peak_time - baseline_time) / 1000 : 0;
  ls.temp_sum        = temp_sum;
  ls.temp_n          = temp_n;
  ls.temp_min_c10    = temp_min_c10;
  ls.temp_max_c10    = temp_max_c10;
  ls.cycle_recorded  = cycle_recorded ? 1 : 0;
  ls.saved_epoch     = ntp_ok ? (uint32_t)time(nullptr) : 0;
  ls.baseline_temp_c10   = baseline_temp_c10;
  ls.mins_since_peaked   = peaked_time ? (millis() - peaked_time) / 60000 : 0;
  ls.prepeak_alert_sent  = prepeak_alert_sent ? 1 : 0;
  ls.levain_target_hit   = levain_target_hit ? 1 : 0;
  ls.levain_hit_min      = levain_hit_min;
  prefs.putBytes("live", &ls, sizeof(ls));
  last_state_save_ms = millis();
}

void clearLiveState() {
  prefs.remove("live");
  last_state_save_ms = millis();
}

// Drop everything about the in-progress cycle and go back to IDLE. Used by
// POST /api/reset and by a mode change (pam: a mode switch requires
// re-calibration). Completed cycle history is NOT touched — that's
// /api/reset_cycles.
void resetToIdle() {
  state = ST_IDLE;
  baseline_dist = 0; peak_rise_mm = 0; baseline_epoch = 0;
  baseline_time = millis(); peak_time = millis();
  temp_sum = 0; temp_n = 0;
  temp_min_c10 = INT16_MAX; temp_max_c10 = INT16_MIN;
  baseline_temp_c10 = INT16_MIN;
  cycle_recorded = false;
  peaked_time = 0;
  prepeak_alert_sent = false;
  predicted_peak_min = 0; predict_n = 0;
  overflow_alerted = false; runaway_alerted = false;
  rate_n = 0; rate_head = 0; rise_rate_x100 = 0;
  levain_target_hit = false; levain_hit_min = 0;
  clearLiveState();
  Serial.println(">> Reset to IDLE");
}

// Returns minutes of downtime if a cycle was resumed, -1 if nothing to resume.
long restoreLiveState() {
  LiveState ls;
  if (prefs.getBytes("live", &ls, sizeof(ls)) != sizeof(ls)) return -1;
  if (ls.state == ST_IDLE || ls.state > ST_FALLING) return -1;
  if (ls.baseline_dist == 0) return -1;

  state          = (State)ls.state;
  baseline_dist  = ls.baseline_dist;
  baseline_epoch = ls.baseline_epoch;
  peak_rise_mm   = ls.peak_rise_mm;
  temp_sum       = ls.temp_sum;
  temp_n         = ls.temp_n;
  temp_min_c10   = ls.temp_min_c10;
  temp_max_c10   = ls.temp_max_c10;
  cycle_recorded = ls.cycle_recorded != 0;
  baseline_temp_c10  = ls.baseline_temp_c10;
  prepeak_alert_sent = ls.prepeak_alert_sent != 0;
  levain_target_hit  = ls.levain_target_hit != 0;
  levain_hit_min     = ls.levain_hit_min;
  peaked_time = (ls.state == ST_PEAKED)
                ? millis() - (unsigned long)ls.mins_since_peaked * 60000UL : 0;

  uint32_t now_epoch = ntp_ok ? (uint32_t)time(nullptr) : 0;
  long downtime_min = -1;

  // Back-dating millis() anchors is safe even when the subtraction wraps:
  // millis() - (millis() - X) == X in unsigned modular arithmetic, which is
  // the same property every millis() timer already relies on.
  if (ntp_ok && ls.baseline_epoch && now_epoch > ls.baseline_epoch) {
    uint32_t elapsed_s = now_epoch - ls.baseline_epoch;
    baseline_time = millis() - (unsigned long)elapsed_s * 1000UL;
    peak_time = (ls.peak_epoch && now_epoch > ls.peak_epoch)
                ? millis() - (unsigned long)(now_epoch - ls.peak_epoch) * 1000UL
                : baseline_time;
    if (ls.saved_epoch && now_epoch > ls.saved_epoch)
      downtime_min = (now_epoch - ls.saved_epoch) / 60;
  } else {
    // No wall clock: resume from the last saved counters. Time spent powered
    // off is invisible, so elapsed will under-report by the outage length.
    baseline_time = millis() - (unsigned long)ls.elapsed_min * 60000UL;
    peak_time     = millis() - (unsigned long)ls.mins_since_peak * 60000UL;
  }

  // Seed the smoothing buffer so the first rise reading isn't garbage.
  for (int i = 0; i < SMOOTH_N; i++) dist_buffer[i] = baseline_dist;
  buffer_full = true;
  return downtime_min < 0 ? 0 : downtime_min;
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

// Starter column height under the sensor, in mm. 0 if geometry is unusable.
int16_t starterHeightMm() {
  if (baseline_dist == 0) return 0;
  int16_t h = (int16_t)jar_height_mm - (int16_t)baseline_dist;
  return h > 0 ? h : 0;
}

// ------------------------------------------------------------ prediction ---
// "Comparable" = temp-stable within itself AND run at roughly the temperature
// we're at now, AND it actually peaked. Cycles that never peaked carry no
// time-to-peak information, so they can't inform a time prediction (they DO
// still inform phase classification, which is why they're stored).
bool cycleComparable(const Cycle& c, float temp_now) {
  if (c.time_to_peak_min == 0) return false;
  if (!cycleTempStable(c)) return false;
  if (isnan(temp_now) || c.avg_temp_c10 == INT16_MIN) return false;
  int16_t now10 = (int16_t)lroundf(temp_now * 10.0f);
  return abs((int)c.avg_temp_c10 - (int)now10) <= TEMP_MATCH_BAND_C10;
}

// Median, not mean: with 3-10 samples one weird cycle (a missed peak, a day
// the jar sat in the sun) would drag a mean badly. Median just ignores it.
uint16_t medianU16(uint16_t* v, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {           // insertion sort, n <= 10
    uint16_t k = v[i]; int8_t j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return (n % 2) ? v[n / 2] : (uint16_t)(((uint32_t)v[n / 2 - 1] + v[n / 2]) / 2);
}

// Recompute predicted_peak_min / predict_n for the current temperature.
void refreshPrediction(float temp_now) {
  uint16_t t2p[CYCLE_SLOTS];
  uint8_t n = 0;
  uint8_t start = (cyc_count == CYCLE_SLOTS) ? cyc_head : 0;
  for (uint8_t i = 0; i < cyc_count; i++) {
    const Cycle& c = cycles[(start + i) % CYCLE_SLOTS];
    if (cycleComparable(c, temp_now)) t2p[n++] = c.time_to_peak_min;
  }
  predict_n = n;
  predicted_peak_min = (n >= MIN_CYCLES_FOR_PREDICT) ? medianU16(t2p, n) : 0;

  Serial.print(">> Prediction: ");
  if (predicted_peak_min) {
    Serial.print(predicted_peak_min); Serial.print(" min from ");
    Serial.print(n); Serial.println(" comparable cycles");
  } else {
    Serial.print("none yet ("); Serial.print(n); Serial.print("/");
    Serial.print(MIN_CYCLES_FOR_PREDICT); Serial.println(" comparable cycles)");
  }
}

// Starter-level verdict across the comparable cycles. Distinct from a single
// cycle's phase: "mature" here demands repeatability, not one good day.
Phase overallPhase(float temp_now, uint8_t* out_n, uint8_t* out_doubled) {
  uint8_t n = 0, doubled = 0, silent = 0, bacterial = 0;
  uint8_t start = (cyc_count == CYCLE_SLOTS) ? cyc_head : 0;
  for (uint8_t i = 0; i < cyc_count; i++) {
    const Cycle& c = cycles[(start + i) % CYCLE_SLOTS];
    if (!cycleTempStable(c)) continue;
    // Temperature match is deliberately NOT required here: phase is about the
    // starter's biology, and excluding cycles by temperature would hide a
    // silent starter just because the kitchen was cold that week.
    n++;
    Phase p = classifyCycle(c);
    if (p == PH_SILENT) silent++;
    else if (p == PH_BACTERIAL) bacterial++;
    if (cyclePct(c) >= (int16_t)DOUBLED_PCT) doubled++;
  }
  if (out_n) *out_n = n;
  if (out_doubled) *out_doubled = doubled;
  if (n < MIN_CYCLES_FOR_PREDICT) return PH_UNKNOWN;
  if (silent * 2 >= n) return PH_SILENT;          // half or more did nothing
  if (doubled == n) return PH_MATURE;             // every one doubled
  if (bacterial * 2 >= n) return PH_BACTERIAL;
  return PH_YEAST;
}

// ------------------------------------------------------- overflow / rate ---
// Least-squares slope over the 5-minute window, in mm/min x100. A regression
// rather than (last-first)/dt because the ToF sensor is noisy at close range
// and endpoint differencing would let one bad reading fake a runaway.
int16_t computeRiseRateX100() {
  if (rate_n < 3) return 0;
  unsigned long now = millis();
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < rate_n; i++) {
    const RatePoint& p = rate_buf[i];
    if (now - p.t > RATE_WINDOW_MS) continue;      // outside the window
    double x = (double)(now - p.t) / -60000.0;     // minutes, negative = older
    double y = (double)p.rise;
    sx += x; sy += y; sxx += x * x; sxy += x * y; n++;
  }
  if (n < 3) return 0;
  double denom = (double)n * sxx - sx * sx;
  if (fabs(denom) < 1e-9) return 0;
  double slope = ((double)n * sxy - sx * sy) / denom;   // mm per minute
  double v = slope * 100.0;
  if (v >  32000) v =  32000;
  if (v < -32000) v = -32000;
  return (int16_t)lround(v);
}

// Two independent safety alerts, both with hysteresis so a starter hovering
// at the threshold doesn't spam Telegram.
void checkOverflow(uint16_t dist, int16_t rise_mm) {
  if (state == ST_IDLE || dist == 0) return;

  // 1. Proximity to the lid — the direct blowout predictor.
  if (!overflow_alerted && dist <= OVERFLOW_DIST_MM) {
    overflow_alerted = true;
    int16_t sh = starterHeightMm();
    String msg = "🚨 Overflow warning — check the jar!\n";
    msg += "Only " + String(dist) + " mm of headroom left under the lid.\n";
    msg += "Risen: " + String(rise_mm) + " mm";
    if (sh > 0) msg += " (" + String(100.0f * rise_mm / sh, 0) + "%)";
    msg += "\nMove it to a bigger vessel or scrape some out.";
    sendTelegram(msg);
  } else if (overflow_alerted && dist >= OVERFLOW_REARM_MM) {
    overflow_alerted = false;   // fell back — re-arm for a genuine second rise
  }

  // 2. Rate of climb — catches a runaway before it reaches the lid.
  if (!runaway_alerted && rise_rate_x100 >= RUNAWAY_RATE_X100) {
    runaway_alerted = true;
    sendTelegram("⚡ Runaway fermentation\n"
                 "Rising " + String(rise_rate_x100 / 100.0f, 1) + " mm/min "
                 "over the last 5 min.\n"
                 "Headroom: " + String(dist) + " mm.\n"
                 "It's going faster than usual — keep an eye on it.");
  } else if (runaway_alerted && rise_rate_x100 <= (int16_t)RUNAWAY_REARM_X100) {
    runaway_alerted = false;
  }
}

// ------------------------------------------------------------ levain (F4) --
// Alert once per calibration when the build reaches the target % rise. The
// state machine deliberately CONTINUES afterwards: pam's levain workflow needs
// both events, so if it later peaks the normal peak alert still fires.
//
// `levain_target_hit` is never cleared on a fall, only at calibration — pam:
// "Don't re-alert if target hit again after a fall."
void checkLevainTarget(int16_t rise_mm) {
  if (mode != MODE_LEVAIN || state == ST_IDLE || levain_target_hit) return;
  int16_t sh = starterHeightMm();
  if (sh <= 0) return;

  int16_t pct = (int16_t)(100L * rise_mm / sh);
  if (pct < (int16_t)levain_target_pct) return;

  levain_target_hit = true;
  levain_hit_min = (uint16_t)((millis() - baseline_time) / 60000);
  sendTelegram("🎯 Target reached (" + String(pct) + "%) at " +
               String(levain_hit_min) + " min — use now");
  Serial.print(">> Levain target hit at "); Serial.print(pct);
  Serial.println("%");
  saveLiveState();
}

// One-shot "mature starter" congratulation. pam's rule: 3 comparable cycles
// that all doubled. Latched in NVS so it fires once per starter, not once per
// reboot — a celebration that repeats is just noise.
void checkMaturity() {
  if (prefs.getUChar("mature", 0)) return;
  uint8_t n = 0, doubled = 0;
  Phase p = overallPhase(last_temp, &n, &doubled);
  if (p == PH_MATURE && n >= MIN_CYCLES_FOR_PREDICT && doubled == n) {
    prefs.putUChar("mature", 1);
    sendTelegram("🎉 Mature starter!\n"
                 "Last " + String(n) + " comparable cycles all doubled.\n"
                 "It's reliable enough to bake on a schedule now.");
    Serial.println(">> Maturity reached");
  }
}

// Record the just-finished cycle. peaked=false means it never reached PEAKED,
// which is itself a signal (a "silent" starter) worth keeping.
//
// Levain builds are deliberately NOT recorded: different flour, hydration and
// quantity from the maintenance starter, so folding them into the same history
// would poison the prediction model with cycles that aren't comparable.
void closeCycle(bool peaked) {
  if (state == ST_IDLE || cycle_recorded) return;
  if (mode == MODE_LEVAIN) { cycle_recorded = true; return; }

  Cycle& c = cycles[cyc_head];
  c.timestamp        = baseline_epoch;
  c.baseline_mm      = baseline_dist;
  c.jar_height_mm    = jar_height_mm;
  c.peak_rise_mm     = peak_rise_mm;
  c.time_to_peak_min = peaked ? (uint16_t)((peak_time - baseline_time) / 60000) : 0;
  c.avg_temp_c10     = temp_n ? (int16_t)lroundf(temp_sum * 10.0f / (float)temp_n)
                              : INT16_MIN;
  c.temp_min_c10     = (temp_min_c10 == INT16_MAX) ? INT16_MIN : temp_min_c10;
  c.temp_max_c10     = temp_max_c10;
  c.baseline_temp_c10 = baseline_temp_c10;
  c.peak_hold_min    = 0;   // filled in later, when PEAKED -> FALLING

  cyc_head = (cyc_head + 1) % CYCLE_SLOTS;
  if (cyc_count < CYCLE_SLOTS) cyc_count++;
  cycle_recorded = true;
  saveCycles();

  Serial.print(">> Cycle recorded: peak ");
  Serial.print(c.peak_rise_mm);
  Serial.print(" mm in ");
  Serial.print(c.time_to_peak_min);
  Serial.println(" min");
}

void calibrate(uint16_t d) {
  if (d == 0) {
    Serial.println(">> Cannot calibrate: sensor out of range");
    sendTelegram("⚠️ Calibration failed — starter too far from sensor. Wait for it to rise, then try again.");
    return;
  }

  // A feed ends whatever cycle was running. If it never peaked, record it
  // anyway (only once it had a fair chance) so silent cycles are visible.
  if (state != ST_IDLE && !cycle_recorded) {
    unsigned long ran_min = (millis() - baseline_time) / 60000;
    if (ran_min >= 60) closeCycle(false);
  }

  baseline_dist = d;
  baseline_time = millis();
  peak_rise_mm = 0;
  peak_time = baseline_time;
  state = ST_INITIAL;
  for (int i = 0; i < SMOOTH_N; i++) dist_buffer[i] = d;
  buffer_full = true;

  baseline_epoch = ntp_ok ? (uint32_t)time(nullptr) : 0;
  temp_sum = 0; temp_n = 0;
  temp_min_c10 = INT16_MAX; temp_max_c10 = INT16_MIN;
  cycle_recorded = false;
  peaked_time = 0;
  prepeak_alert_sent = false;
  overflow_alerted = false;
  runaway_alerted = false;
  rate_n = 0; rate_head = 0; rise_rate_x100 = 0;
  levain_target_hit = false;
  levain_hit_min = 0;

  Serial.print(">> CALIBRATED at ");
  Serial.print(d);
  Serial.println(" mm");

  float temp = sht31_ok ? sht31.readTemperature() : NAN;
  baseline_temp_c10 = isnan(temp) ? INT16_MIN : (int16_t)lroundf(temp * 10.0f);

  // Predict this cycle's peak from comparable past cycles at this temperature.
  refreshPrediction(temp);

  String msg = "🌱 Starter calibrated\n";
  msg += "Baseline: " + String(d) + " mm";
  if (!isnan(temp)) msg += "\nTemp: " + String(temp, 1) + "°C";
  int16_t sh = starterHeightMm();
  if (sh > 0) msg += "\nStarter height: " + String(sh) + " mm";
  if (predicted_peak_min > 0) {
    msg += "\n\n🔮 Expected peak in ~" + String(predicted_peak_min) + " min"
           " (from " + String(predict_n) + " similar cycles)";
  }
  sendTelegram(msg);

  saveLiveState();
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
        saveLiveState();
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
        peaked_time = millis();
        closeCycle(true);
        checkMaturity();
        saveLiveState();
      }
      break;
    case ST_PEAKED:
      if (peak_rise_mm - rise_mm >= FALL_FROM_PEAK_MM) {
        state = ST_FALLING;
        Serial.println(">> State: FALLING");
        // Peak-hold duration separates a bacterial spike-and-collapse from a
        // yeast-driven sustained peak. Back-fill it on the cycle just stored.
        //
        // MUST be guarded on starter mode: a levain build never stored a
        // cycle, so cyc_head still points past the PREVIOUS starter cycle and
        // this would overwrite that cycle's hold time with the levain's.
        if (peaked_time && mode == MODE_STARTER && cyc_count) {
          uint8_t last = (cyc_head + CYCLE_SLOTS - 1) % CYCLE_SLOTS;
          cycles[last].peak_hold_min = (uint16_t)((millis() - peaked_time) / 60000);
          saveCycles();
        }
        sendTelegram("📉 Past peak, falling\n"
                     "Down " + String(peak_rise_mm - rise_mm) + " mm from peak.\n"
                     "Feed soon.");
        saveLiveState();
      }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------- dashboard ---
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
header{display:flex;align-items:baseline;gap:12px;flex-wrap:wrap;padding:18px 20px 10px}
h1{font-size:18px;margin:0;font-weight:600}
h2{font-size:13px;letter-spacing:.07em;text-transform:uppercase;color:var(--dim);
   margin:20px 2px 8px;font-weight:600}
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
input{background:#14110e;color:var(--ink);border:1px solid var(--line);
      border-radius:8px;padding:8px 10px;width:80px;font-size:14px}
#msg{font-size:13px;color:var(--dim)}
table{width:100%;border-collapse:collapse;font-size:13px;
      background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}
th{text-align:left;font-size:11px;letter-spacing:.06em;text-transform:uppercase;
   color:var(--dim);font-weight:600;padding:9px 10px;border-bottom:1px solid var(--line)}
td{padding:8px 10px;border-bottom:1px solid #262019;font-variant-numeric:tabular-nums}
tr:last-child td{border-bottom:none}
.tag{font-size:11px;padding:2px 7px;border-radius:99px;background:var(--line);color:var(--dim)}
.tag.good{background:#2c3a24;color:var(--ok)}
.tag.none{background:#3a2a24;color:var(--warn)}
/* Mode-dependent visibility. body gets .m-starter or .m-levain; pam wants a
   task-focused levain view with the prediction/phase/history clutter gone. */
body.m-levain .starter-only{display:none}
body.m-starter .levain-only{display:none}
input:disabled{opacity:.45;cursor:not-allowed}
button:disabled{opacity:.45;cursor:not-allowed}
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
    <div class="card"><div class="k">Rise (100% = doubled)</div>
      <div class="v"><span id="risepct">--</span><span class="u">%</span></div></div>
    <div class="card"><div class="k">Peak so far</div>
      <div class="v"><span id="peak">--</span><span class="u">mm</span></div></div>
    <div class="card"><div class="k">Elapsed</div>
      <div class="v"><span id="elapsed">--</span></div></div>
    <div class="card"><div class="k">Temperature</div>
      <div class="v"><span id="temp">--</span><span class="u">&deg;C</span></div></div>
    <div class="card"><div class="k">Humidity</div>
      <div class="v"><span id="hum">--</span><span class="u">%</span></div></div>
    <div class="card"><div class="k">Headroom to lid</div>
      <div class="v"><span id="dist">--</span><span class="u">mm</span></div></div>
    <div class="card"><div class="k">Starter height</div>
      <div class="v"><span id="sh">--</span><span class="u">mm</span></div></div>
    <div class="card starter-only"><div class="k">Peak expected in</div>
      <div class="v"><span id="peakin">--</span></div></div>
    <div class="card"><div class="k">Rise rate (5 min)</div>
      <div class="v"><span id="rate">--</span><span class="u">mm/min</span></div></div>
    <div class="card levain-only"><div class="k">Target</div>
      <div class="v"><span id="tgt">--</span><span class="u">%</span></div></div>
  </div>
  <div id="phase" class="starter-only"
       style="margin-top:10px;font-size:13px;color:var(--dim)"></div>

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
    <button id="reset">Reset to idle</button>
    <span id="msg"></span>
  </div>

  <h2>Mode</h2>
  <div class="row">
    <button id="mstarter">Starter</button>
    <button id="mlevain">Levain / dough</button>
    <span id="modemsg" style="font-size:13px;color:var(--dim)"></span>
  </div>
  <div id="modeline" style="font-size:13px;color:var(--dim);margin-top:6px"></div>
  <div style="font-size:12px;color:var(--dim);margin-top:6px">
    Switching mode resets tracking to IDLE &mdash; re-calibrate after you switch.
    Starter mode tracks the full rise/peak/fall cycle and trains the peak
    prediction. Levain mode alerts at your target % and is <em>not</em> saved
    into cycle history &mdash; different flour and hydration would skew the model.
  </div>

  <h2>Settings</h2>
  <div class="row">
    <label for="jar" style="font-size:13px;color:var(--dim)">
      Sensor face &rarr; jar bottom (mm)</label>
    <input id="jar" type="number" min="20" max="500">
    <button id="savejar">Save</button>
    <span id="jarmsg" style="font-size:13px;color:var(--dim)"></span>
  </div>
  <div class="row">
    <label for="target" style="font-size:13px;color:var(--dim)">
      Levain target rise (%)</label>
    <input id="target" type="number" min="10" max="400">
    <button id="savetarget">Save</button>
    <span id="tgtmsg" style="font-size:13px;color:var(--dim)"></span>
  </div>

  <div class="starter-only">
    <h2>Completed cycles</h2>
    <table id="cyc"><thead><tr>
      <th>Fed</th><th>Peak</th><th>Rise %</th><th>To peak</th><th>Temp</th><th></th>
    </tr></thead><tbody></tbody></table>
    <div class="row">
      <button id="wipe">Wipe cycle history</button>
      <span id="wipemsg" style="font-size:13px;color:var(--dim)"></span>
    </div>
  </div>
</main>

<footer id="foot"></footer>

<script>
const $=id=>document.getElementById(id);
let hist={s:[],t0:0}, now=null, jarTouched=false, targetTouched=false;
// Mirrored so the chart can redraw the target line without waiting for the
// next /api/history poll (which is only once a minute).
let targetPct=null, levMode=null;

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
    $('sh').textContent   = now.starter_height? now.starter_height : '--';
    $('peakin').textContent = now.peak_in_min==null? '--' : fmtDur(now.peak_in_min);
    $('rate').textContent = now.rise_rate==null? '--' : now.rise_rate.toFixed(2);
    if(!jarTouched) $('jar').value = now.jar_height;
    if(!targetTouched) $('target').value = now.target_pct;
    $('tgt').textContent = now.target_pct;

    const lev = now.mode==='levain';
    // Drives the .starter-only / .levain-only CSS rules.
    document.body.className = lev? 'm-levain' : 'm-starter';
    $('mstarter').className = lev? '' : 'primary';
    $('mlevain').className  = lev? 'primary' : '';
    // pam: target field only interactive in LEVAIN mode.
    $('target').disabled = !lev;
    $('savetarget').disabled = !lev;
    $('modeline').innerHTML = lev
      ? ('Levain mode &mdash; target '+now.target_pct+'%'
         + (now.target_hit? ' &middot; <b>reached</b> after '+fmtDur(now.target_hit_min)
                          : ' &middot; not reached yet'))
      : 'Starter mode &mdash; tracking the full cycle';
    if(targetPct!==now.target_pct){ targetPct=now.target_pct; draw(); }
    if(levMode!==lev){ levMode=lev; draw(); }
    $('age').textContent  = 'updated '+new Date().toLocaleTimeString();
    $('foot').innerHTML = 'ESP32 up '+fmtDur(Math.floor(now.uptime_s/60))
      +' &middot; SHT31 '+(now.sht31?'ok':'<span class=bad>missing</span>')
      +' &middot; VL53L0X '+(now.vl53?'ok':'<span class=bad>missing</span>')
      +' &middot; free heap '+(now.heap/1024).toFixed(0)+' KB'
      +' &middot; '+now.history_n+' history points'
      +' &middot; '+now.cycles_n+' saved cycles';
  }catch(e){ $('age').textContent='connection lost'; }
}

async function loadHistory(){
  try{ const r=await fetch('/api/history',{cache:'no-store'});
       hist=await r.json(); draw(); }catch(e){}
}

async function loadPrediction(){
  try{
    const r=await fetch('/api/prediction',{cache:'no-store'});
    const j=await r.json();
    let s='Phase: <b>'+j.overall_phase+'</b>';
    s+=' &middot; '+j.stable_cycles+' temp-stable cycles, '+j.doubled_cycles+' doubled';
    if(j.predicted_peak_min>0)
      s+=' &middot; predicting peak at '+fmtDur(j.predicted_peak_min)
        +' from '+j.predict_n+' comparable cycles';
    else
      s+=' &middot; no prediction yet ('+j.predict_n+'/'+j.min_cycles+' comparable cycles)';
    $('phase').innerHTML=s;
  }catch(e){}
}

async function loadCycles(){
  try{
    const r=await fetch('/api/history_cycles',{cache:'no-store'});
    const j=await r.json();
    const tb=$('cyc').querySelector('tbody'); tb.innerHTML='';
    if(!j.cycles.length){
      tb.innerHTML='<tr><td colspan="6" style="color:var(--dim)">'+
        'No completed cycles yet. One gets saved each time the starter peaks.</td></tr>';
      return;
    }
    j.cycles.slice().reverse().forEach(c=>{
      const sh=c.jar_height_mm-c.baseline_mm;
      const pct=sh>0? (100*c.peak_rise_mm/sh) : null;
      const tr=document.createElement('tr');
      tr.innerHTML =
        '<td>'+(c.timestamp? new Date(c.timestamp*1000)
                 .toLocaleString([], {month:'short',day:'numeric',
                                      hour:'2-digit',minute:'2-digit'}) : '&mdash;')+'</td>'+
        '<td>'+c.peak_rise_mm+' mm</td>'+
        '<td>'+(pct==null?'&mdash;':pct.toFixed(0)+'%')+'</td>'+
        '<td>'+(c.time_to_peak_min? fmtDur(c.time_to_peak_min):'&mdash;')+'</td>'+
        '<td>'+(c.avg_temp_c==null?'&mdash;':c.avg_temp_c.toFixed(1)+'\u00b0')+'</td>'+
        '<td>'+(c.time_to_peak_min===0
                 ? '<span class="tag none">no peak</span>'
                 : (pct!=null&&pct>=100? '<span class="tag good">doubled</span>'
                                       : '<span class="tag">peaked</span>'))+'</td>';
      tb.appendChild(tr);
    });
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

  x.strokeStyle='#332c24'; x.fillStyle='#a2937f'; x.font='11px sans-serif'; x.lineWidth=1;
  for(let i=0;i<=4;i++){
    const v=rmin+(rmax-rmin)*i/4, y=Yr(v);
    x.beginPath(); x.moveTo(L,y); x.lineTo(L+pw,y); x.stroke();
    x.textAlign='right'; x.fillText(v.toFixed(0),L-6,y+4);
    const tvv=tmin+(tmax-tmin)*i/4;
    x.textAlign='left'; x.fillStyle='#5fb0d6';
    x.fillText(tvv.toFixed(0)+'\u00b0',L+pw+6,y+4); x.fillStyle='#a2937f';
  }
  x.textAlign='center';
  for(let i=0;i<=4;i++){
    const tt=t0+(t1-t0)*i/4;
    const mins=Math.round((t1-tt)/60);
    let lab = hist.epoch ? new Date((hist.epoch+(tt-hist.now))*1000)
                             .toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'})
                         : (mins===0?'now':'-'+fmtDur(mins));
    x.fillText(lab,X(tt),T+ph+16);
  }
  x.strokeStyle='#5fb0d6'; x.lineWidth=1.5; x.beginPath(); let pen=false;
  tp.forEach((v,i)=>{ if(v==null){pen=false;return;}
    const px=X(t[i]),py=Yt(v); pen?x.lineTo(px,py):x.moveTo(px,py); pen=true; });
  x.stroke();
  x.strokeStyle='#e8a33d'; x.lineWidth=2; x.beginPath();
  rise.forEach((v,i)=>{ const px=X(t[i]),py=Yr(v); i?x.lineTo(px,py):x.moveTo(px,py); });
  x.stroke();
  x.lineTo(X(t1),Yr(rmin)); x.lineTo(X(t0),Yr(rmin)); x.closePath();
  x.fillStyle='rgba(232,163,61,.13)'; x.fill();

  // Levain target as a dashed horizontal line, in mm converted from the % so
  // it shares the rise axis. Only drawn in levain mode, and only when the
  // starter height is known — without it the % has no mm equivalent.
  if(levMode && targetPct && now && now.starter_height>0){
    const tmm = targetPct/100*now.starter_height;
    if(tmm>=rmin && tmm<=rmax){
      x.save();
      x.setLineDash([6,4]); x.strokeStyle='#7ec86a'; x.lineWidth=1.5;
      x.beginPath(); x.moveTo(L,Yr(tmm)); x.lineTo(L+pw,Yr(tmm)); x.stroke();
      x.setLineDash([]);
      x.fillStyle='#7ec86a'; x.font='11px sans-serif'; x.textAlign='left';
      x.fillText('target '+targetPct+'% ('+tmm.toFixed(0)+' mm)',L+4,Yr(tmm)-5);
      x.restore();
    }
  }

  $('span').textContent='window: '+fmtDur(Math.round((t1-t0)/60));
}

$('cal').onclick=async()=>{
  $('msg').textContent='setting baseline…';
  try{ const r=await fetch('/api/calibrate',{method:'POST'});
       const j=await r.json();
       $('msg').textContent = j.ok ? 'baseline set at '+j.baseline+' mm'
                                   : 'failed: '+j.error;
  }catch(e){ $('msg').textContent='failed: no response'; }
  tick(); loadCycles();
};

$('reset').onclick=async()=>{
  if(!confirm('Reset to IDLE? The in-progress cycle is discarded.')) return;
  try{ await fetch('/api/reset',{method:'POST'}); $('msg').textContent='reset to idle';
  }catch(e){ $('msg').textContent='failed: no response'; }
  tick();
};

async function setConfig(qs,msgEl,okText){
  try{ const r=await fetch('/api/config?'+qs,{method:'POST'});
       const j=await r.json();
       $(msgEl).textContent = j.ok? okText(j) : ('failed: '+(j.error||''));
       return j.ok;
  }catch(e){ $(msgEl).textContent='failed: no response'; return false; }
}

// A mode change resets tracking to IDLE, so confirm before discarding a
// cycle that is actually running.
async function switchMode(to){
  const lev = now && now.mode==='levain';
  if((to==='levain')===!!lev) return;              // already in that mode
  if(now && now.state!=='IDLE' &&
     !confirm('Switching mode resets tracking to IDLE and discards the '+
              'in-progress cycle. Continue?')) return;
  await setConfig('mode='+to,'modemsg',
                  ()=>to+' mode \u2014 re-calibrate to start tracking');
  tick(); loadCycles(); loadPrediction();
}
$('mstarter').onclick=()=>switchMode('starter');
$('mlevain').onclick =()=>switchMode('levain');

$('target').oninput=()=>{targetTouched=true;};
$('savetarget').onclick=async()=>{
  const v=parseInt($('target').value,10);
  if(!(v>=10&&v<=400)){ $('tgtmsg').textContent='target must be 10\u2013400%'; return; }
  if(await setConfig('levain_target='+v,'tgtmsg',j=>'target set to '+j.target_pct+'%')){
    targetTouched=false;
    targetPct=v; draw();
  }
  tick();
};

$('wipe').onclick=async()=>{
  if(!confirm('Erase all saved cycles? Peak prediction and phase '+
              'classification start over from nothing.')) return;
  try{ const r=await fetch('/api/reset_cycles',{method:'POST'});
       const j=await r.json();
       $('wipemsg').textContent = j.ok? ('wiped '+j.cleared+' cycles') : 'failed';
  }catch(e){ $('wipemsg').textContent='failed: no response'; }
  tick(); loadCycles(); loadPrediction();
};

$('jar').oninput=()=>{jarTouched=true;};
$('savejar').onclick=async()=>{
  const v=parseInt($('jar').value,10);
  if(!(v>=20&&v<=500)){ $('jarmsg').textContent='must be 20–500 mm'; return; }
  try{ const r=await fetch('/api/config?jar='+v,{method:'POST'});
       const j=await r.json();
       $('jarmsg').textContent = j.ok? 'saved — rise % now uses '+j.jar_height+' mm'
                                     : 'failed';
       jarTouched=false;
  }catch(e){ $('jarmsg').textContent='failed: no response'; }
  tick(); loadCycles();
};

$('csv').onclick=()=>{
  const rows=[['seconds_since_boot','distance_mm','rise_mm','temp_c','humidity_pct','state']];
  (hist.s||[]).forEach(p=>rows.push([p[0],p[1],p[2],
      p[3]===-32768?'':(p[3]/10).toFixed(1), p[4]===255?'':p[4], p[5]]));
  const blob=new Blob([rows.map(r=>r.join(',')).join('\n')],{type:'text/csv'});
  const a=document.createElement('a');
  a.href=URL.createObjectURL(blob); a.download='sourdough.csv'; a.click();
};

tick(); loadHistory(); loadCycles(); loadPrediction();
setInterval(tick,2000);
setInterval(loadHistory,60000);
setInterval(loadCycles,60000);
setInterval(loadPrediction,60000);
addEventListener('resize',draw);
</script></body></html>)HTML";

void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleNow() {
  int16_t sh = starterHeightMm();
  String j = "{";
  j += "\"state\":\"" + String(stateNames[state]) + "\"";
  j += ",\"dist\":" + String(last_dist);
  j += ",\"rise\":" + String(last_rise);
  j += ",\"peak_rise\":" + String(peak_rise_mm);
  j += ",\"baseline\":" + String(baseline_dist);
  j += ",\"jar_height\":" + String(jar_height_mm);
  j += ",\"starter_height\":" + String(sh);
  if (state != ST_IDLE && sh > 0 && last_dist > 0) {
    // 100% == doubled: rise measured against the starter column, not the gap.
    j += ",\"rise_pct\":" + String(100.0f * (float)last_rise / (float)sh, 1);
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
  j += ",\"cycles_n\":" + String(cyc_count);
  j += ",\"predicted_peak_min\":" + String(predicted_peak_min);
  j += ",\"predict_n\":" + String(predict_n);
  if (state != ST_IDLE) j += ",\"rise_rate\":" + String(rise_rate_x100 / 100.0f, 2);
  else                  j += ",\"rise_rate\":null";
  j += ",\"overflow_alerted\":" + String(overflow_alerted ? "true" : "false");
  j += ",\"runaway_alerted\":" + String(runaway_alerted ? "true" : "false");
  j += ",\"mode\":\"" + String(mode == MODE_LEVAIN ? "levain" : "starter") + "\"";
  j += ",\"target_pct\":" + String(levain_target_pct);
  j += ",\"target_hit\":" + String(levain_target_hit ? "true" : "false");
  j += ",\"target_hit_min\":" + String(levain_hit_min);
  if (predicted_peak_min > 0 && state != ST_IDLE) {
    long mins = (long)((millis() - baseline_time) / 60000);
    long remain = (long)predicted_peak_min - mins;
    j += ",\"peak_in_min\":" + String(remain > 0 ? remain : 0);
  } else {
    j += ",\"peak_in_min\":null";
  }
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

// Oldest first. 10 cycles max, so a plain String is fine here.
void handleHistoryCycles() {
  uint8_t start = (cyc_count == CYCLE_SLOTS) ? cyc_head : 0;
  String j = "{\"n\":" + String(cyc_count) + ",\"cycles\":[";
  for (uint8_t i = 0; i < cyc_count; i++) {
    const Cycle& c = cycles[(start + i) % CYCLE_SLOTS];
    if (i) j += ',';
    j += "{\"timestamp\":" + String(c.timestamp);
    j += ",\"baseline_mm\":" + String(c.baseline_mm);
    j += ",\"jar_height_mm\":" + String(c.jar_height_mm);
    j += ",\"peak_rise_mm\":" + String(c.peak_rise_mm);
    j += ",\"time_to_peak_min\":" + String(c.time_to_peak_min);
    j += ",\"avg_temp_c\":" + (c.avg_temp_c10 == INT16_MIN ? String("null")
                                : String(c.avg_temp_c10 / 10.0f, 1));
    j += ",\"temp_min_c\":" + (c.temp_min_c10 == INT16_MIN ? String("null")
                                : String(c.temp_min_c10 / 10.0f, 1));
    j += ",\"temp_max_c\":" + (c.temp_max_c10 == INT16_MIN ? String("null")
                                : String(c.temp_max_c10 / 10.0f, 1));
    j += "}";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
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

// Accepts any combination of jar / mode / target, so the dashboard can send
// just the field the user touched.
void handleConfig() {
  bool any = false;

  if (server.hasArg("jar")) {
    long v = server.arg("jar").toInt();
    if (v < 20 || v > 500) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"jar out of range (20-500)\"}");
      return;
    }
    jar_height_mm = (uint16_t)v;
    prefs.putUShort("jar_mm", jar_height_mm);
    Serial.print(">> Jar height set to "); Serial.print(jar_height_mm);
    Serial.println(" mm");
    any = true;
  }

  if (server.hasArg("mode")) {
    String m = server.arg("mode");
    Mode requested;
    if (m == "starter")     requested = MODE_STARTER;
    else if (m == "levain") requested = MODE_LEVAIN;
    else {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"mode must be starter|levain\"}");
      return;
    }
    // pam: "Mode change resets state to IDLE and requires re-calibration."
    // Only on an actual change — re-sending the current mode shouldn't nuke a
    // running cycle. A starter cycle that had a fair run is banked first so
    // switching to levain doesn't silently discard real training data.
    if (requested != mode) {
      if (mode == MODE_STARTER && state != ST_IDLE && !cycle_recorded) {
        unsigned long ran_min = (millis() - baseline_time) / 60000;
        if (ran_min >= 60) closeCycle(false);
      }
      mode = requested;
      prefs.putUChar("mode", (uint8_t)mode);
      resetToIdle();
      Serial.print(">> Mode set to "); Serial.print(m);
      Serial.println(" — state reset to IDLE, re-calibrate");
    }
    any = true;
  }

  // pam's spec names this parameter `levain_target`. `target` stays accepted
  // as an alias so anything already scripted against the older name keeps
  // working; both write the same NVS key.
  if (server.hasArg("levain_target") || server.hasArg("target")) {
    long v = server.hasArg("levain_target") ? server.arg("levain_target").toInt()
                                            : server.arg("target").toInt();
    if (v < LEVAIN_TARGET_PCT_MIN || v > LEVAIN_TARGET_PCT_MAX) {
      server.send(400, "application/json",
                  "{\"ok\":false,\"error\":\"levain_target out of range (10-400)\"}");
      return;
    }
    levain_target_pct = (uint16_t)v;
    prefs.putUShort("lev_pct", levain_target_pct);
    levain_target_hit = false;   // new target deserves a fresh alert
    Serial.print(">> Levain target set to "); Serial.print(levain_target_pct);
    Serial.println("%");
    any = true;
  }

  if (!any) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"nothing to set\"}");
    return;
  }
  server.send(200, "application/json",
              "{\"ok\":true,\"jar_height\":" + String(jar_height_mm) +
              ",\"mode\":\"" + String(mode == MODE_LEVAIN ? "levain" : "starter") +
              "\",\"target_pct\":" + String(levain_target_pct) + "}");
}

// The whole inference, exposed for inspection. When a prediction looks wrong
// this is how you find out which cycles it trusted and why.
void handlePrediction() {
  uint8_t n = 0, doubled = 0;
  Phase p = overallPhase(last_temp, &n, &doubled);

  String j = "{";
  j += "\"predicted_peak_min\":" + String(predicted_peak_min);
  j += ",\"predict_n\":" + String(predict_n);
  j += ",\"min_cycles\":" + String(MIN_CYCLES_FOR_PREDICT);
  j += ",\"warn_before_min\":" + String(PREDICT_WARN_MIN);
  j += ",\"prepeak_alert_sent\":" + String(prepeak_alert_sent ? "true" : "false");
  j += ",\"overall_phase\":\"" + String(phaseNames[p]) + "\"";
  j += ",\"stable_cycles\":" + String(n);
  j += ",\"doubled_cycles\":" + String(doubled);
  j += ",\"mature_alerted\":" + String(prefs.getUChar("mature", 0) ? "true" : "false");
  j += ",\"cycles\":[";
  uint8_t start = (cyc_count == CYCLE_SLOTS) ? cyc_head : 0;
  for (uint8_t i = 0; i < cyc_count; i++) {
    const Cycle& c = cycles[(start + i) % CYCLE_SLOTS];
    if (i) j += ',';
    j += "{\"timestamp\":" + String(c.timestamp);
    j += ",\"peak_rise_mm\":" + String(c.peak_rise_mm);
    j += ",\"rise_pct\":" + String(cyclePct(c));
    j += ",\"time_to_peak_min\":" + String(c.time_to_peak_min);
    j += ",\"peak_hold_min\":" + String(c.peak_hold_min);
    j += ",\"phase\":\"" + String(phaseNames[classifyCycle(c)]) + "\"";
    j += ",\"temp_stable\":" + String(cycleTempStable(c) ? "true" : "false");
    j += ",\"comparable\":" + String(cycleComparable(c, last_temp) ? "true" : "false");
    j += "}";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleReset() {
  resetToIdle();
  server.send(200, "application/json", "{\"ok\":true}");
}

// pam: wipe the trained-on-nothing state once real data starts arriving.
// Clears the cycle ring AND the maturity latch — otherwise a starter that had
// already earned its 🎉 could never earn it again on real data.
// LAN-trusted, no auth, per pam's "auth-optional for now".
void handleResetCycles() {
  uint8_t had = cyc_count;
  memset(cycles, 0, sizeof(cycles));
  cyc_count = 0; cyc_head = 0;
  saveCycles();
  prefs.remove("mature");
  predicted_peak_min = 0; predict_n = 0;
  Serial.print(">> Cycle history wiped ("); Serial.print(had);
  Serial.println(" cycles)");
  server.send(200, "application/json",
              "{\"ok\":true,\"cleared\":" + String(had) + "}");
}

void handleNotFound() { server.send(404, "text/plain", "not found"); }

// ------------------------------------------------------------------ setup --
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Sourdough Sensor v0.9 ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin();

  if (sht31.begin(0x44)) { sht31_ok = true; Serial.println("SHT31: OK"); }
  else Serial.println("SHT31: NOT FOUND");

  if (vl53.begin()) { vl53_ok = true; Serial.println("VL53L0X: OK"); }
  else Serial.println("VL53L0X: NOT FOUND");

  prefs.begin("sourdough", false);
  if (prefs.getUChar("schema", 0) != NVS_SCHEMA) {
    Serial.println("NVS: new/changed schema, starting clean");
    prefs.clear();
    prefs.putUChar("schema", NVS_SCHEMA);
  }
  jar_height_mm = prefs.getUShort("jar_mm", JAR_INTERIOR_HEIGHT_MM_DEFAULT);
  mode = (Mode)prefs.getUChar("mode", (uint8_t)MODE_STARTER);
  levain_target_pct = prefs.getUShort("lev_pct", LEVAIN_TARGET_PCT_DEFAULT);
  loadCycles();
  Serial.print("NVS: jar height "); Serial.print(jar_height_mm);
  Serial.print(" mm, "); Serial.print(cyc_count); Serial.println(" saved cycles");

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

    // NTP must settle BEFORE restoring state — elapsed time is anchored to it.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 20 && time(nullptr) < 1700000000; i++) delay(200);
    ntp_ok = time(nullptr) > 1700000000;
    Serial.println(ntp_ok ? "NTP: OK" : "NTP: not synced (elapsed time will under-report)");
  } else {
    Serial.println("\nWiFi: FAILED (continuing without Telegram)");
  }

  long downtime = restoreLiveState();

  if (wifi_ok) {
    String msg = "🔌 Sourdough monitor online\n"
                 "IP: " + WiFi.localIP().toString() + "\n"
                 "Dashboard: http://" + WiFi.localIP().toString() + "/\n";
    if (downtime >= 0) {
      msg += "\n♻️ Resumed cycle in progress\n";
      msg += "State: " + String(stateNames[state]) + "\n";
      msg += "Baseline: " + String(baseline_dist) + " mm\n";
      msg += "Elapsed: " + String((millis() - baseline_time) / 60000) + " min";
      if (downtime >= (long)STALE_RESUME_MINUTES) {
        msg += "\n⚠️ Powered off ~" + String(downtime) + " min — the rise curve "
               "has a gap and the peak may have been missed.";
      }
    } else {
      msg += "Press the button after feeding to calibrate.";
    }
    sendTelegram(msg);
  }
  if (downtime >= 0) {
    Serial.print(">> Resumed "); Serial.print(stateNames[state]);
    Serial.print(", downtime "); Serial.print(downtime); Serial.println(" min");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/now", HTTP_GET, handleNow);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/history_cycles", HTTP_GET, handleHistoryCycles);
  server.on("/api/prediction", HTTP_GET, handlePrediction);
  server.on("/api/calibrate", HTTP_POST, handleCalibrate);
  server.on("/api/config", HTTP_POST, handleConfig);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/reset_cycles", HTTP_POST, handleResetCycles);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server: started on port 80");

  Serial.println("\nt(s)\telapsed\tstate\tT\tH\tdist\trise\tpeak");
}

// ------------------------------------------------------------------- loop --
void loop() {
  // These must run every iteration — the sampling gate below `return`s,
  // which would otherwise starve the web server.
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

  // Cycle temperature stats — needed for feature 3's +/-3 C stability rule.
  if (state != ST_IDLE && !isnan(temp)) {
    temp_sum += temp; temp_n++;
    int16_t t10 = (int16_t)lroundf(temp * 10.0f);
    if (t10 < temp_min_c10) temp_min_c10 = t10;
    if (t10 > temp_max_c10) temp_max_c10 = t10;
  }

  last_temp = temp; last_hum = hum; last_dist = dist; last_rise = rise_mm;

  // Feed the 5-minute rate window every ~10 s, then evaluate the safety
  // alerts. Both only make sense once a baseline exists.
  if (state != ST_IDLE && dist > 0 && millis() - last_rate_ms >= 10000) {
    last_rate_ms = millis();
    rate_buf[rate_head] = { millis(), rise_mm };
    rate_head = (rate_head + 1) % RATE_SLOTS;
    if (rate_n < RATE_SLOTS) rate_n++;
    rise_rate_x100 = computeRiseRateX100();
  }
  checkOverflow(dist, rise_mm);
  checkLevainTarget(rise_mm);

  if (millis() - last_log_ms >= LOG_INTERVAL_MS || hist_count == 0) {
    last_log_ms = millis();
    historyPush(millis() / 1000, dist, rise_mm, temp, hum, (uint8_t)state);
  }

  // Pre-peak warning. Fires once, PREDICT_WARN_MIN before the predicted peak,
  // and only while still climbing — if it already peaked the alert is moot.
  if (predicted_peak_min > 0 && !prepeak_alert_sent &&
      (state == ST_INITIAL || state == ST_RISING)) {
    unsigned long mins = (millis() - baseline_time) / 60000;
    if (mins + PREDICT_WARN_MIN >= predicted_peak_min) {
      prepeak_alert_sent = true;
      long remain = (long)predicted_peak_min - (long)mins;
      if (remain < 0) remain = 0;
      sendTelegram("⏰ Peak expected in ~" + String(remain) + " min\n"
                   "Elapsed: " + String(mins) + " min\n"
                   "Risen: " + String(rise_mm) + " mm so far\n"
                   "Based on " + String(predict_n) + " similar cycles.");
      saveLiveState();
    }
  }

  // Heartbeat save so a power cut loses at most STATE_SAVE_INTERVAL_MS of
  // peak/elapsed detail. State transitions save immediately regardless.
  if (state != ST_IDLE && millis() - last_state_save_ms >= STATE_SAVE_INTERVAL_MS) {
    saveLiveState();
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
