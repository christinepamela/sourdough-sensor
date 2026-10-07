/*
  Sourdough Starter Monitor — v1.6
  ================================
  Baseline: pam's v0.4 (archived). v0.5 added the web dashboard.

  Note on versioning: v1.0 was skipped (Buzz numbered v0.9 -> v1.1 directly).

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

  WHAT CHANGED IN v1.1 - STAIRCASE PEAK DETECTION + HARDWARE (Buzz)
  -----------------------------------------------------------------
  + Rewritten peak detection: rise curves are STAIRCASES, not bell curves.
    ST_PEAKED is now SOFT and reversible — alerts fire, but cycle is only
    recorded once a fall is confirmed. If starter climbs again, silently
    returns to RISING. Validated against pam's day2-day8 CSVs.
  + New tunable thresholds (all POST-able via /api/config):
    - rising_threshold_mm = 6 (was 3)
    - min_meaningful_rise_mm = 8 (was 10)
    - min_sustained_minutes = 30
    - peak_stable_minutes = 45
    - peak_fall_pct = 15 (fall threshold as % of peak, clamped 3-15mm)
    - secondary_rise_mm = 3
  + Warmup period (10 min) after calibration: no transitions, no alerts.
  + GPIO 18 second button for feeding pause; GPIO 17 external status LED.
  + Sensor auto-recovery: reinit -> I2C bus reset -> ESP32 restart.
  + Temperature warnings: cold < 24°C for > 60 min, hot > 30°C for > 30 min.
  + Weak cycle detection: alerts on declining peaks across 3+ cycles.
  + Cycle history management: individual delete + mark-invalid endpoints.
  + JAR_INTERIOR_HEIGHT_MM_DEFAULT corrected to 175 (was 165 experimental).

  WHAT CHANGED IN v1.6 - STATUS OFF-BY-ONE FIX, RISE NUMBERING, CSV EXPORT
  -------------------------------------------------------------------------
  BUG FIX:
  + v1.5's cycle status was off by one. highest_state_this_cycle stores the
    raw State enum (ST_IDLE=0, ST_INITIAL=1 .. ST_FALLING=4), but the
    dashboard's statusLabel[] is 0-based starting at "initial". So a 100%+
    cycle that reached PEAKED (enum 3) displayed as "falling", and a cycle
    that reached FALLING (enum 4) had no array slot and showed "undefined".
    Fixed at the source: c.status = highest_state_this_cycle - ST_INITIAL.
    Cycles already saved in the ring keep their old (wrong) label until
    they age out; new cycles are correct immediately.

  BUG FIX:
  + Every staircase recovery (PEAKED/FALLING -> RISING again) was announced
    as "Secondary rise detected", every time, no matter how many times it
    happened in one cycle — so a 3rd or 4th recovery still read as the 2nd.
    Now counted per-cycle (recovery_rise_n, reset at calibration alongside
    highest_state_this_cycle) and reported with its real ordinal: "3rd rise
    detected", "4th rise detected", etc. The first INITIAL->RISING is rise
    #1, so the first recovery is rise #2 — matches how you were counting it.

  NEW FEATURE:
  + "⬇️ Download results (CSV)" button next to Wipe cycle history. Exports
    the completed-cycles table (fed time, peak, rise %, time to peak, rate,
    temps, peak-hold, status) before you wipe — so wrapping up a test (e.g.
    a discard trial) leaves a record instead of disappearing when you start
    tracking the mother starter fresh.

  WHAT CHANGED IN v1.5 - ACCURATE CYCLE STATUS, RATE, TREND CHARTS (Pam)
  -----------------------------------------------------------------------
  + Cycle history no longer mislabels a cut-short cycle as "no peak". Each
    cycle now records the highest state it actually reached: initial,
    rising, peaked, or falling — independent of whether the fall was ever
    confirmed before the next feed. A cycle fed right after PEAKED shows
    "peaked" with a real time-to-peak, not blank.
  + Time-to-peak is captured at the moment PEAKED is confirmed (not only
    when a fall later confirms the cycle), and survives secondary rises.
  + New "Rate" column: peak rise / time-to-peak, in mm/hr.
  + New "Maturity trend" section below Completed cycles: three small
    sparkline charts (peak rise, time-to-peak, rise rate) across recent
    cycles, so progress is visible at a glance. Fully separate from the
    existing 24h chart — doesn't touch it.
  + NOTE: the Cycle struct grew by one byte (status field), which changes
    its size in NVS. On first boot after flashing v1.5 the saved cycle
    history will reset to empty (loadCycles() detects the size mismatch
    and starts clean) — the 24h chart and live state are unaffected.

  WHAT CHANGED IN v1.4 - FREEZE TEMP/HUM DURING FEEDING MODE (Pam)
  -----------------------------------------------------------------
  + Feeding mode now also freezes temperature and humidity in the chart
    history (in addition to distance, which v1.2a already handled).
  + When you enter feeding mode, the current temp/hum values are
    snapshotted and used for all history logs until you exit.
  + Chart stays clean when jar is briefly out of the styrofoam during
    feed (previously showed a spike as kitchen air was read).
  + Dashboard still shows real-time temp/hum (only history is frozen).

  WHAT CHANGED IN v1.2a - CRITICAL BUG FIXES (Pam)
  -------------------------------------------------
  BUG FIXES:
  + Feeding mode was decorative — didn't actually protect data.
    Buzz's v1.1 delivered feeding_mode that:
      - Skipped state transitions (worked)
      - Skipped temp alerts (worked)
      - Skipped feed-due alert (worked)
    But DIDN'T:
      - Skip peak_rise_mm update (spike from lifting jar = false 140mm peak)
      - Skip historyPush (polluted the 24hr chart, made Y-axis unusable)
      - Skip checkOverflow (could fire false runaway alert)
      - Skip checkLevainTarget (could fire false target-hit alert)
      - Skip rate window (polluted rise rate calculations)
    Real incident 2026-08-26 Day 3 feed: pressed toggle correctly,
    jar was lifted, sensor spiked 158mm → 25mm, recorded as 140mm rise.
    All above now properly gated by feeding_mode.

  + During feeding mode, historyPush now logs (baseline_dist, 0) instead
    of the actual spike readings. Chart stays flat during feed, and you
    can visually see the pause.

  NEW FEATURE:
  + "Clear chart" button on dashboard + POST /api/wipe_history endpoint.
    Wipes the 24-hour ring buffer without affecting cycle history or
    current state. Useful for recovering after a disturbance.

  WHAT CHANGED IN v1.2 - DASHBOARD FEED BUTTON + DAY X TRACKING (Pam)
  -------------------------------------------------------------------
  BUG FIXES:
  + checkFeedButton() and updateLeds() were defined but never called in
    loop(). Physical GPIO 18 button and status LED were dead code.
    Now called every loop iteration.

  NEW FEATURES:
  + Dashboard "Toggle feeding mode" button — pause tracking without
    opening the styrofoam box first (which was defeating the pause).
    Big orange button turns green when in feeding mode; state indicator
    shows "FEEDING (paused)" in orange.
  + Day X starter age tracker: user sets Day 1 at start of new starter,
    auto-increments every 24 hours (based on NTP). Dashboard shows current
    day plus phase guidance (bacterial / silent / yeast-establishing /
    approaching maturity / mature). Persisted in NVS.
  + New API endpoints:
    - POST /api/feeding_toggle — same behavior as physical GPIO 18 button
    - POST /api/starter_day?day=X — set starter age (0 = disabled)
  + /api/now response now includes feeding_mode and starter_day fields.

  IMPLEMENTED BY PAM DIY (no Buzz cost).

  UNCHANGED ON PURPOSE
  --------------------
  The state machine (IDLE/INITIAL/RISING/PEAKED/FALLING), its thresholds,
  the distance smoothing, and the five original Telegram messages are the
  v0.4 behaviour verbatim. Serial output format is unchanged.

  Board: ESP32 DevKitC-32 (WROOM-32).  I2C SDA=21 SCL=22,
  SHT31 @0x44, VL53L0X @0x29, calibrate button GPIO19 (INPUT_PULLUP),
  feed button GPIO18 (INPUT_PULLUP, v1.1), external LED GPIO17 (v1.1).

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
const int BUTTON_PIN  = 19;   // calibrate / "just fed"
const int FEED_BTN_PIN = 18;  // v1.1: feeding-pause button
const int LED_PIN      = 17;  // v1.1: external status LED (220R to GND)
const int LED_BUILTIN_PIN = 2;// onboard LED, used for a slow health heartbeat

// Measure this from the SENSOR FACE (lid underside, lid closed) to the
// INSIDE BOTTOM of the jar. That is the distance the sensor would read with
// an empty jar, and it is what makes rise % mean "doubled".
// pam, 2026-08-23: 750 ml regular-mouth Mason jar, ~175 mm interior. The
// earlier 165 was an experimental value. This is only the DEFAULT — the live
// value is runtime-settable and persisted, so it never needs a reflash.
const uint16_t JAR_INTERIOR_HEIGHT_MM_DEFAULT = 175;

// ============================ PEAK DETECTION (v1.1) ==========================
// Rewritten against 8 days of real hardware data (pam's day2-day8 CSVs).
//
// WHAT THE DATA ACTUALLY SHOWS, and why this is not the brief's algorithm:
//
//   The rise curves are STAIRCASES, not bell curves. The day5 reference cycle
//   stalls for 258 minutes at ~10 mm before climbing on to its true 29 mm
//   peak. day6 stalls 248 minutes before its 12 mm peak.
//
//   The brief (Section 1.1 Stage 3) confirms a peak after "no new maximum for
//   45 min". Those stalls are five times longer than that window, so that rule
//   declares the peak mid-rise — reproducing the exact v0.9 bug it was meant to
//   fix. A window long enough to survive them (>260 min) would report every
//   peak four hours late.
//
//   The brief's second gate, "rise rate < 0.3 mm/h", cannot rescue it: measured
//   on this data the 90-min slope during day5's pre-peak stall (median
//   +1.42 mm/h) OVERLAPS the slope at its true plateau (median +0.48, p90
//   +1.27). The two situations are not separable by slope, so that gate is not
//   implemented — it would only add false confidence.
//
// THE RULE THIS CODE USES INSTEAD:
//   A stall is not evidence of a peak. Only a sustained FALL is.
//
//   So ST_PEAKED is soft and reversible. It means "holding, probably done" and
//   it alerts, but the cycle is only WRITTEN TO HISTORY once a fall is
//   confirmed (see closeCycle callers). If the starter climbs again, the state
//   machine silently returns to RISING and the peak keeps tracking upward.
//   On day5 this steps 12 -> 16 -> 20 -> 24 -> 28 mm and lands on the real
//   29 mm peak instead of stopping at 12.
//
// All five constants are POST-able via /api/config (Section 9).
// -----------------------------------------------------------------------------

// Stage 1 — rise validation. 6 mm rather than the brief's 10 mm because day6's
// entire real peak is 12 mm and only clears 10 mm for 31 minutes; a 10 mm gate
// nearly misses a genuine doubling. Sample-to-sample noise is p99 = 3 mm, so
// requiring consecutive confirmations kills noise triggering without costing
// sensitivity.
int16_t  rising_threshold_mm    = 6;
const uint8_t RISING_CONSEC     = 3;    // consecutive samples above threshold

// A peak must be at least this tall to be a peak at all. 8 mm, not the brief's
// 10 mm, for the same day6 reason.
int16_t  min_meaningful_rise_mm = 8;

// ...and the rise must have been held that high this long before we will even
// consider a peak. Filters bacterial spikes that shoot up and collapse.
uint16_t min_sustained_minutes  = 30;

// Stage 3 — how long with no new maximum before we call it "holding".
uint16_t peak_stable_minutes    = 45;

// A new max only resets the stability timer if it beats the old one by this
// much. Without a margin, 1 mm of noise perpetually resets the timer and the
// peak is never confirmed.
const int16_t NEW_MAX_MARGIN_MM = 2;

// Stage 4 — fall confirmation, proportional to peak height (Section 1.4).
// fall_threshold = clamp(15% of peak, 3 mm, 15 mm)
uint16_t peak_fall_pct          = 15;
const int16_t FALL_MIN_MM       = 3;
const int16_t FALL_MAX_MM       = 15;
uint16_t fall_stable_minutes    = 20;

// Stage 5 — secondary rise. Measured against the peak AS IT WAS WHEN PEAKED
// WAS DECLARED, not against the running maximum.
//
// This distinction is load-bearing. If it compared against the running max,
// 1 mm-per-sample creep would keep raising the max while the state stayed
// PEAKED, and day5 would sit at "peaked, 18 mm" while the starter climbed to
// 29 mm — the same silent-stall failure as v0.9, just harder to see.
int16_t  secondary_rise_mm      = 3;

// Section 1.6 — warmup. The first samples after a calibrate are noisy (the
// user's hand is still near the jar). Log them, but no transitions, no alerts.
uint16_t warmup_minutes         = 10;

// Superseded v0.9 constants, kept only so the old peak-hold back-fill and the
// levain path keep compiling. Not used by the new detector.
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
// v1.1 Section 1.3: 3 mm/min is a NORMAL fermentation rate, so the old value
// cried wolf. Runaway should mean a genuine emergency.
const int16_t  RUNAWAY_RATE_X100  = 500;  // 5.00 mm/min over a 5-min window
const uint16_t RUNAWAY_REARM_X100 = 250;  // re-arm below 2.50 mm/min
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

// ==================== TEMPERATURE WARNINGS (v1.1, Section 3.6) ===============
// The brief says "temp < 24 C for > 2 h". Measured against pam's day7 file —
// the very cold-shock case it is supposed to catch — that rule NEVER FIRES:
// the longest unbroken sub-24 C stretch there is 68 minutes, because the
// reading oscillates back above the line. At 60 minutes it fires at t=1.47h.
//
// The window is contiguous-only on purpose. Accumulating total cold minutes
// would fire on day5/day6, which spent 145/214 minutes below 24 C while
// happily rising to full peaks (yeast momentum) — those are healthy cycles and
// warning about them would train pam to ignore the alert.
float    cold_warn_c        = 24.0f;
uint16_t cold_warn_minutes  = 60;
float    hot_warn_c         = 30.0f;
uint16_t hot_warn_minutes   = 30;
// Below this, fermentation is effectively paused rather than merely slow.
float    cold_paused_c      = 22.0f;

// ==================== WEAK CYCLE DETECTION (v1.1, Section 4.4) ===============
// pam's day8 file is the reference: a rescue feed that never exceeded 4 mm of
// rise over 20 hours at good temperature. That is what "the starter has
// failed" looks like, and the point is to say so BEFORE it is unrecoverable.
const int16_t  WEAK_PEAK_MM         = 8;   // a cycle peaking under this is weak
const uint8_t  WEAK_DECLINE_CYCLES  = 3;   // consecutive shrinking peaks
const uint16_t WEAK_MIN_RUN_MIN     = 360; // only judge a cycle after 6 h

// ==================== FEED SAFETY NET (v1.1, Section 3.2) ====================
// Secondary, time-based trigger: whichever fires first, peak or clock.
uint16_t feed_interval_hours = 24;

// ==================== SENSOR RECOVERY (v1.1, Section 2.1) ====================
// pam's day6 file contains a real 63-minute VL53L0X lockup that only a manual
// power cycle cleared, while the SHT31 kept working — so this is a ToF/I2C
// state problem, not a power problem. Escalating ladder, gentlest first.
const uint8_t  SENSOR_FAIL_CONSEC   = 5;    // 5 zero readings = "lost"
const uint16_t SENSOR_L2_AFTER_S    = 30;   // then: library re-init
const uint16_t SENSOR_L3_AFTER_S    = 120;  // then: full I2C bus reset
const uint16_t SENSOR_L4_AFTER_S    = 900;  // then: ESP.restart(), last resort
const uint8_t  SENSOR_FAILS_PER_DAY_ALERT = 5;

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
// Pam v1.5: highest state actually reached this cycle, independent of the
// live `state` variable (which can drop back to RISING on a secondary rise).
// Used so a cycle cut short by a feed is labeled by what really happened —
// "peaked", not "no peak" — instead of only recording completed falls.
uint8_t highest_state_this_cycle = ST_INITIAL;
// First moment PEAKED was confirmed this cycle. Unlike peaked_time (which
// resets to 0 on a secondary rise), this is captured once and kept, so
// "time to peak" survives even if fed before the fall confirms, or if a
// secondary rise happens afterwards.
unsigned long first_peaked_time = 0;
bool     cycle_recorded = false;  // has this cycle been written to history?

// v1.6: counts how many times THIS cycle has recovered from PEAKED/FALLING
// back into RISING (a "staircase" starter). The first INITIAL->RISING is
// rise #1, so the first recovery is rise #2, the next is #3, etc. Reset at
// every calibration alongside highest_state_this_cycle.
uint8_t  recovery_rise_n = 0;

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

// ------------------------------------------------------ v1.1 peak detector --
// Stage 2/3 bookkeeping. peak_rise_mm above is still the running maximum;
// these track HOW LONG it has stood and what it was when PEAKED was declared.
unsigned long last_newmax_ms   = 0;   // last time the max improved by >= margin
unsigned long sustained_since_ms = 0; // when rise last crossed min_meaningful
bool          sustained_active = false;
unsigned long fall_since_ms    = 0;   // when the confirmed fall began
bool          falling_pending  = false;
int16_t       peak_at_confirm  = 0;   // peak as of the PEAKED transition
uint8_t       rise_consec      = 0;   // consecutive samples over threshold
unsigned long warmup_until_ms  = 0;   // no transitions/alerts before this

// ------------------------------------------------------- v1.1 feeding mode --
// A manual pause for feeding: state frozen, no transitions, no alerts. Second
// press (or the timeout) re-calibrates at the new baseline.
bool          feeding_mode      = false;
unsigned long feeding_since_ms  = 0;
bool          feed_last_button  = HIGH;
unsigned long feed_last_change  = 0;
const uint16_t FEEDING_TIMEOUT_MIN = 20;
const uint16_t FEEDING_WARN_MIN    = 18;   // LED starts blinking here
bool          feeding_warned    = false;

// Pam v1.4: freeze temp/humidity readings during feeding mode. Captured on
// entry, used for historyPush so the chart doesn't spike when the jar is
// out of the box. Actual temp/humidity still read for last_temp (dashboard).
float feeding_frozen_temp = NAN;
float feeding_frozen_hum  = NAN;

// ------------------------------------------------- v1.1 temperature warnings --
unsigned long cold_since_ms = 0;  bool cold_active = false, cold_warned = false;
unsigned long hot_since_ms  = 0;  bool hot_active  = false, hot_warned  = false;
bool          paused_warned = false;

// --------------------------------------------------------- v1.1 feed timer --
bool feed_due_alerted = false;

// ----------------------------------------------------- v1.1 sensor recovery --
// Escalating recovery for the VL53L0X lockup seen in pam's day6 data.
uint16_t      sensor_zero_run    = 0;   // consecutive zero-distance reads
bool          sensor_lost        = false;
unsigned long sensor_lost_ms     = 0;
uint8_t       sensor_recovery_level = 0;  // 0=none,2=reinit,3=i2c,4=restart
uint16_t      sensor_fail_count  = 0;     // failures since boot
uint32_t      sensor_bad_samples = 0;     // for the uptime percentage
uint32_t      sensor_all_samples = 0;

// --------------------------------------------------- v1.1 invalid-cycle marks --
// A bitmask over the cycle ring, stored under its own NVS key so the Cycle
// struct — and therefore pam's existing saved history — is untouched.
uint16_t cycle_invalid_mask = 0;

// --------------------------------------------------- Pam v1.1a: starter age --
// Day X tracker for the starter's biological age (Day 1 = start of new starter).
// Persisted in NVS. Auto-increments each calendar day the device is running
// (based on NTP time), and user-editable via /api/config?starter_day=X.
// Setting starter_day=0 pauses auto-increment (useful for testing).
uint16_t starter_day = 0;              // 0 = not tracking, 1+ = day N
uint32_t last_day_check_epoch = 0;     // last time we checked for day rollover

// ------------------------------------------------------------- v1.1 LED UI --
unsigned long led_last_toggle_ms = 0;
bool          led_state = false;

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

// Clear the v1.1 peak-detector bookkeeping.
//
// Extracted into one function because it has to happen in BOTH calibrate()
// and resetToIdle(), and a detector that keeps last cycle's timers would
// confirm a peak within seconds of the new baseline. Keeping the two call
// sites in sync by hand is exactly the drift that caused the v0.9
// handleReset() bug.
void resetDetectorState() {
  last_newmax_ms     = millis();
  sustained_since_ms = 0;
  sustained_active   = false;
  fall_since_ms      = 0;
  falling_pending    = false;
  peak_at_confirm    = 0;
  rise_consec        = 0;
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
  // v1.1 state — must be cleared here too, or /api/reset leaves the detector
  // holding the old cycle's timers.
  resetDetectorState();
  warmup_until_ms = 0;
  feeding_mode = false; feeding_warned = false;
  feed_due_alerted = false;
  cold_active = false; cold_warned = false;
  hot_active  = false; hot_warned  = false;
  paused_warned = false;
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

// ==================== SENSOR RECOVERY (v1.1, Section 2.1) ====================
// pam's day6 file contains a real 63-minute VL53L0X lockup: distance pinned at
// 0 while the SHT31 on the SAME I2C bus kept reporting normally. Only a manual
// power cycle cleared it. That rules out power and points at either the ToF's
// internal state or the bus, so the ladder tries the cheap fixes first and
// only reboots as a last resort.
//
// Escalation is by ELAPSED TIME rather than attempt count: retrying a hung
// sensor in a tight loop just burns I2C transactions. Each level runs once.
void escalateSensorRecovery() {
  unsigned long down_s = (millis() - sensor_lost_ms) / 1000;

  // ---- Level 2: re-initialise the library ---------------------------------
  if (sensor_recovery_level < 2 && down_s >= SENSOR_L2_AFTER_S) {
    sensor_recovery_level = 2;
    Serial.println(">> Sensor recovery L2: re-init VL53L0X");
    vl53_ok = vl53.begin();
    Serial.println(vl53_ok ? ">> L2 begin() OK" : ">> L2 begin() failed");
    return;
  }

  // ---- Level 3: reset the whole I2C bus, then re-init ----------------------
  if (sensor_recovery_level < 3 && down_s >= SENSOR_L3_AFTER_S) {
    sensor_recovery_level = 3;
    Serial.println(">> Sensor recovery L3: I2C bus reset");
    Wire.end();
    delay(50);
    Wire.begin();
    delay(50);
    vl53_ok = vl53.begin();
    // The SHT31 shares the bus, so it has to come back too.
    sht31_ok = sht31.begin(0x44);
    Serial.println(vl53_ok ? ">> L3 bus reset OK" : ">> L3 bus reset failed");
    return;
  }

  // ---- Level 4: reboot. Tell the user BEFORE going down. ------------------
  if (sensor_recovery_level < 4 && down_s >= SENSOR_L4_AFTER_S) {
    sensor_recovery_level = 4;
    Serial.println(">> Sensor recovery L4: restarting ESP32");
    // The in-progress cycle is already in NVS, so the reboot resumes it.
    saveLiveState();
    sendTelegram("🔄 Sensor unrecoverable after " + String(down_s / 60) +
                 " min — restarting the monitor.\n"
                 "The cycle in progress is saved and will resume automatically.");
    delay(1000);
    ESP.restart();
  }
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

// ==================== TEMPERATURE WARNINGS (v1.1, Section 3.6) ===============
// Contiguous-window logic — see the constants block for why "total minutes
// below 24 C" would fire on the healthy day5/day6 cycles and train pam to
// ignore the alert.
//
// Each warning latches until the temperature comes back across the line, so a
// jar hovering at 23.9 C produces one message, not forty.
void checkTempWarnings(float temp) {
  if (isnan(temp) || state == ST_IDLE || feeding_mode) return;
  unsigned long now = millis();

  // --- cold: fermentation delayed --------------------------------------
  if (temp < cold_warn_c) {
    if (!cold_active) { cold_active = true; cold_since_ms = now; }
    if (!cold_warned && (now - cold_since_ms) / 60000 >= cold_warn_minutes) {
      cold_warned = true;
      String m = "🥶 Temperature cool — " + String(temp, 1) + " C\n";
      m += "Below " + String(cold_warn_c, 0) + " C for over " +
           String(cold_warn_minutes) + " min. Fermentation will be slow.";
      if (temp < cold_paused_c)
        m += "\n\nUnder " + String(cold_paused_c, 0) +
             " C the yeast is effectively paused, not dead — warm it up and it "
             "should come back.";
      sendTelegram(m);
      Serial.print(">> TEMP WARNING: cold "); Serial.println(temp, 1);
    }
  } else {
    cold_active = false; cold_warned = false;
  }

  // --- hot: over-fermentation risk -------------------------------------
  if (temp > hot_warn_c) {
    if (!hot_active) { hot_active = true; hot_since_ms = now; }
    if (!hot_warned && (now - hot_since_ms) / 60000 >= hot_warn_minutes) {
      hot_warned = true;
      sendTelegram("🥵 Temperature warm — " + String(temp, 1) + " C\n"
                   "Above " + String(hot_warn_c, 0) + " C for over " +
                   String(hot_warn_minutes) + " min. Risk of over-fermentation; "
                   "it will peak sooner than usual.");
      Serial.print(">> TEMP WARNING: hot "); Serial.println(temp, 1);
    }
  } else {
    hot_active = false; hot_warned = false;
  }
}

// ==================== FEED SAFETY NET (v1.1, Section 3.2) ====================
// Dual-trigger feeding recommendation: the peak-based path lives in the state
// machine, this is the time-based backstop. Whichever fires first wins.
// Deliberately fires even from IDLE-adjacent quiet states — a starter that
// never visibly peaked is exactly the one you can forget to feed.
void checkFeedDue() {
  if (state == ST_IDLE || feed_due_alerted || feeding_mode) return;
  unsigned long hours = (millis() - baseline_time) / 3600000UL;
  if (hours < feed_interval_hours) return;
  feed_due_alerted = true;
  sendTelegram("🍽️ " + String(hours) + " hours since the last feed\n"
               "Consider feeding regardless of where the curve is.\n"
               "Peak so far: " + String(peak_rise_mm) + " mm.");
  Serial.print(">> Feed due: "); Serial.print(hours); Serial.println(" h elapsed");
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

// ==================== WEAK CYCLE DETECTION (v1.1, Section 4.4) ===============
// pam's first starter died and the code said nothing. The whole point here is
// to speak up BEFORE the starter is unrecoverable, so this runs on every
// completed cycle rather than waiting for a verdict.
//
// Two independent signals, both grounded in pam's day8 file (a rescue feed
// that never exceeded 4 mm of rise in 20 hours at good temperature):
//
//   1. The latest cycle barely moved at all      -> weak on its own terms
//   2. N consecutive cycles each smaller than    -> declining trend, the
//      the one before                               pattern that killed #1
//
// Invalid-marked cycles are skipped: a cycle the user flagged as bogus (jar
// bumped, sensor glitch) must not be able to fake a decline.
//
// Latched in NVS so it fires once per decline, not once per cycle forever.
void checkWeakCycles() {
  if (mode != MODE_STARTER || cyc_count == 0) return;

  uint8_t start = (cyc_count == CYCLE_SLOTS) ? cyc_head : 0;

  // Gather valid peaks, oldest -> newest.
  int16_t peaks[CYCLE_SLOTS];
  uint8_t n = 0;
  for (uint8_t i = 0; i < cyc_count; i++) {
    uint8_t idx = (start + i) % CYCLE_SLOTS;
    if (cycle_invalid_mask & (1u << idx)) continue;
    peaks[n++] = cycles[idx].peak_rise_mm;
  }
  if (n == 0) return;

  bool latched = prefs.getUChar("weak", 0) != 0;

  // --- Signal 2: consecutive decline ---------------------------------------
  bool declining = false;
  if (n >= WEAK_DECLINE_CYCLES) {
    declining = true;
    for (uint8_t i = n - WEAK_DECLINE_CYCLES; i + 1 < n; i++) {
      if (peaks[i + 1] >= peaks[i]) { declining = false; break; }
    }
  }

  // --- Signal 1: the newest cycle is inside the noise floor ----------------
  bool tiny = peaks[n - 1] < WEAK_PEAK_MM;

  if (!declining && !tiny) {
    // Healthy again — clear the latch so a future decline can alert.
    if (latched) prefs.putUChar("weak", 0);
    return;
  }
  if (latched) return;   // already told them; don't nag every cycle

  prefs.putUChar("weak", 1);

  String msg = "⚠️ Starter looks weak\n";
  if (declining) {
    msg += "Last " + String(WEAK_DECLINE_CYCLES) + " peaks got smaller each time (";
    for (uint8_t i = n - WEAK_DECLINE_CYCLES; i < n; i++) {
      msg += String(peaks[i]) + "mm";
      if (i + 1 < n) msg += " -> ";
    }
    msg += ").\n";
  } else {
    msg += "Last cycle only reached " + String(peaks[n - 1]) +
           " mm — barely above sensor noise.\n";
  }
  msg += "\nTry:\n"
         "• Bigger feed ratio (1:3:3 or 1:4:4)\n"
         "• Discard more before feeding\n"
         "• Somewhere warmer (27-28 C)\n"
         "• A little bread flour alongside the whole wheat";
  sendTelegram(msg);

  Serial.print(">> WEAK CYCLE detected (");
  Serial.print(declining ? "declining trend" : "peak below noise floor");
  Serial.println(")");
}

// v1.6: "2nd", "3rd", "4th", "11th", "22nd", ... English ordinal suffix.
const char* ordinalSuffix(uint8_t n) {
  if (n % 100 >= 11 && n % 100 <= 13) return "th";
  switch (n % 10) {
    case 1: return "st";
    case 2: return "nd";
    case 3: return "rd";
    default: return "th";
  }
}

// Record the just-finished cycle. status reflects the highest state it
// actually reached (initial/rising/peaked/falling) — a cycle cut short by
// a feed right after peaking is labeled "peaked", not lumped in with one
// that never rose at all.
//
// Levain builds are deliberately NOT recorded: different flour, hydration and
// quantity from the maintenance starter, so folding them into the same history
// would poison the prediction model with cycles that aren't comparable.
void closeCycle() {
  if (state == ST_IDLE || cycle_recorded) return;
  if (mode == MODE_LEVAIN) { cycle_recorded = true; return; }

  Cycle& c = cycles[cyc_head];
  c.timestamp        = baseline_epoch;
  c.baseline_mm      = baseline_dist;
  c.jar_height_mm    = jar_height_mm;
  c.peak_rise_mm     = peak_rise_mm;
  // v1.6: statusLabel[] on the dashboard is 0-based (0=initial..3=falling)
  // but the State enum is 1-based (ST_INITIAL=1..ST_FALLING=4) because
  // ST_IDLE=0 sits in front of it. Storing the raw enum value put every
  // cycle's label one slot off — a 100%+ "peaked" cycle (enum 3) indexed
  // statusLabel[3]=='falling', and a genuinely-falling cycle (enum 4) had no
  // slot at all and rendered "undefined". Subtracting ST_INITIAL re-bases it.
  c.status           = highest_state_this_cycle - ST_INITIAL;
  c.time_to_peak_min = (highest_state_this_cycle >= ST_PEAKED)
                        ? (uint16_t)((first_peaked_time - baseline_time) / 60000) : 0;
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
    if (ran_min >= 60) closeCycle();
  }

  baseline_dist = d;
  baseline_time = millis();
  peak_rise_mm = 0;
  peak_time = baseline_time;
  state = ST_INITIAL;
  highest_state_this_cycle = ST_INITIAL;   // v1.5
  first_peaked_time = 0;                   // v1.5
  recovery_rise_n = 0;                     // v1.6
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

  // ---- v1.1 detector state. All of this MUST reset with the baseline, or
  // the new cycle inherits the last one's peak timers and confirms instantly.
  resetDetectorState();
  warmup_until_ms = millis() + (unsigned long)warmup_minutes * 60000UL;
  feed_due_alerted = false;
  cold_active = false; cold_warned = false;
  hot_active  = false; hot_warned  = false;
  paused_warned = false;

  Serial.print(">> CALIBRATED at ");
  Serial.print(d);
  Serial.print(" mm — warmup ");
  Serial.print(warmup_minutes);
  Serial.println(" min (logging only, no alerts)");

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

// ==================== FEEDING MODE (v1.1, Section 2.3) =======================
// Second physical button on GPIO 18. While feeding, the jar is open, the
// sensor sees your hand, and the starter is being replaced wholesale — every
// reading is meaningless. So: freeze the state machine, suppress alerts, keep
// logging (the disturbance is visible in the chart, marked by the state).
//
// Press 1 -> enter. Press 2 -> exit AND re-calibrate at the new baseline,
// because after a feed the baseline has by definition changed.
// Timeout at 20 min so a forgotten press doesn't silently stop monitoring.

void enterFeedingMode() {
  feeding_mode     = true;
  feeding_since_ms = millis();
  feeding_warned   = false;
  // Pam v1.4: snapshot temp/hum so the chart doesn't spike during feed
  feeding_frozen_temp = last_temp;
  feeding_frozen_hum  = last_hum;
  Serial.println(">> FEEDING MODE: entered (state frozen, alerts suppressed)");
  sendTelegram("🥄 Feeding mode\n"
               "Monitoring paused. Press the feed button again when you're "
               "done and I'll re-baseline automatically.\n"
               "(Auto-exits after " + String(FEEDING_TIMEOUT_MIN) + " min.)");
}

// auto == true when the timeout fired rather than a button press.
void exitFeedingMode(bool automatic) {
  feeding_mode   = false;
  feeding_warned = false;
  Serial.print(">> FEEDING MODE: exited (");
  Serial.print(automatic ? "timeout" : "button");
  Serial.println(") — re-calibrating");

  uint16_t d = readDistance();
  if (d == 0) {
    // Can't set a baseline without a reading. Say so rather than silently
    // resuming against a stale baseline that no longer matches the jar.
    Serial.println(">> Feeding exit: sensor out of range, cannot re-baseline");
    sendTelegram("⚠️ Feeding finished, but the sensor can't see the starter.\n"
                 "Monitoring is resumed against the OLD baseline — press "
                 "calibrate once the reading is good.");
    return;
  }
  calibrate(d);   // sends its own "calibrated" message and re-arms warmup
}

void checkFeedButton() {
  bool now_b = digitalRead(FEED_BTN_PIN);
  unsigned long now = millis();
  if (feed_last_button == HIGH && now_b == LOW && (now - feed_last_change) > 200) {
    feed_last_change = now;
    if (feeding_mode) exitFeedingMode(false);
    else              enterFeedingMode();
  }
  feed_last_button = now_b;

  // Timeout handling, including the warning blink window.
  if (feeding_mode) {
    unsigned long mins = (now - feeding_since_ms) / 60000;
    if (!feeding_warned && mins >= FEEDING_WARN_MIN) {
      feeding_warned = true;
      Serial.println(">> FEEDING MODE: timeout approaching (LED blinking)");
    }
    if (mins >= FEEDING_TIMEOUT_MIN) exitFeedingMode(true);
  }
}

// ==================== STATUS LEDs (v1.1, Section 2.3) ========================
// GPIO 17 (external) carries the state the user needs while standing at the
// jar; GPIO 2 (onboard) is a slow health heartbeat so a hung board is obvious
// without opening the dashboard.
//
//   solid            feeding mode
//   fast blink       feeding mode, about to time out
//   double-ish blink sensor lost / recovering
//   slow breath      peaked — come deal with the jar
//   off              normal running
void updateLeds() {
  unsigned long now = millis();
  unsigned int period = 0;      // 0 == steady
  bool steady_on = false;

  if (feeding_mode) {
    if (feeding_warned) period = 200;     // urgent: about to auto-exit
    else                steady_on = true;
  } else if (sensor_lost) {
    period = 100;                          // fast flicker: something is wrong
  } else if (state == ST_PEAKED) {
    period = 1000;                         // calm pulse: ready for attention
  }

  if (period == 0) {
    digitalWrite(LED_PIN, steady_on ? HIGH : LOW);
    led_state = steady_on;
  } else if (now - led_last_toggle_ms >= period) {
    led_last_toggle_ms = now;
    led_state = !led_state;
    digitalWrite(LED_PIN, led_state ? HIGH : LOW);
  }

  // Onboard heartbeat: one short flash every 2 s means the loop is alive.
  static unsigned long hb = 0;
  static bool hb_on = false;
  unsigned int hb_period = hb_on ? 50 : 1950;
  if (now - hb >= hb_period) {
    hb = now;
    hb_on = !hb_on;
    digitalWrite(LED_BUILTIN_PIN, hb_on ? HIGH : LOW);
  }
}

// Fall threshold proportional to peak height (Section 1.4). A static 5 mm was
// wrong at both ends: too twitchy for a 29 mm peak, too deaf for an 8 mm one.
int16_t fallThresholdMm() {
  long t = (long)peak_rise_mm * (long)peak_fall_pct / 100L;
  if (t < FALL_MIN_MM) t = FALL_MIN_MM;
  if (t > FALL_MAX_MM) t = FALL_MAX_MM;
  return (int16_t)t;
}

// Section 3.1 — the peak message depends on how developed the starter is.
// "Ready to use or feed" is actively dangerous advice for a 3-day-old starter.
String peakGuidance() {
  uint8_t n = 0, doubled = 0;
  Phase p = overallPhase(last_temp, &n, &doubled);
  if (cyc_count == 0)
    return "Starter still developing — feed within 4 hours.";
  if (cyc_count < MIN_CYCLES_FOR_PREDICT)
    return "Keep building maturity — feed within 4 hours.";
  if (p == PH_MATURE && doubled == n && n > 0)
    return "Mature starter — ready to use or feed.";
  if (doubled == 0)
    return "Peaked, but it hasn't doubled yet — keep feeding daily to develop yeast.";
  return "Feed within 4 hours.";
}

// Back-fill peak-hold on the most recently stored cycle. Separates a bacterial
// spike-and-collapse from a yeast-driven sustained peak.
//
// MUST be guarded on starter mode: a levain build never stored a cycle, so
// cyc_head still points past the PREVIOUS starter cycle and this would
// overwrite that cycle's hold time with the levain's.
void backfillPeakHold() {
  if (peaked_time && mode == MODE_STARTER && cyc_count) {
    uint8_t last = (cyc_head + CYCLE_SLOTS - 1) % CYCLE_SLOTS;
    cycles[last].peak_hold_min = (uint16_t)((millis() - peaked_time) / 60000);
    saveCycles();
  }
}

// ============================ THE v1.1 STATE MACHINE =========================
// Stages map to the brief's Section 1.1, with the two documented deviations
// (no slope gate; PEAKED is soft). Read the big comment on the constants
// block above for why.
//
//   INITIAL --(stage 1: rise sustained)--> RISING
//   RISING  --(stage 3: max stood still)--> PEAKED   [alerts, no history write]
//   PEAKED  --(stage 4: fall confirmed)---> FALLING  [writes history HERE]
//   PEAKED/FALLING --(stage 5: new max)---> RISING   [silent recovery]
//
void updateState(int16_t rise_mm) {
  unsigned long now = millis();
  unsigned long mins_elapsed = (now - baseline_time) / 60000;

  // ---- Warmup (Section 1.6): log, but do not act. -------------------------
  if (warmup_until_ms && (long)(now - warmup_until_ms) < 0) return;

  // ---- Feeding mode (Section 2.3): state frozen by the user. --------------
  if (feeding_mode) return;

  // ---- Stage 2: running maximum, with a noise margin on the timer. --------
  // peak_rise_mm itself is updated in loop(); here we only decide whether the
  // improvement was big enough to count as "still climbing".
  static int16_t last_timer_peak = 0;
  if (peak_rise_mm >= last_timer_peak + NEW_MAX_MARGIN_MM) {
    last_timer_peak = peak_rise_mm;
    last_newmax_ms  = now;
  }

  // ---- Stage 1: has the rise been meaningful, continuously? ---------------
  if (rise_mm >= min_meaningful_rise_mm) {
    if (!sustained_active) { sustained_active = true; sustained_since_ms = now; }
  } else {
    sustained_active = false;
  }

  // ---- Stage 5: secondary rise. Checked BEFORE the per-state logic so a
  // climbing starter escapes PEAKED/FALLING immediately.
  // Compared against peak_at_confirm, NOT the running max — see constants.
  if ((state == ST_PEAKED || state == ST_FALLING) &&
      rise_mm >= peak_at_confirm + secondary_rise_mm) {
    if (state == ST_PEAKED) backfillPeakHold();
    state = ST_RISING;
    last_newmax_ms  = now;
    falling_pending = false;
    peaked_time     = 0;
    // The first INITIAL->RISING was rise #1, so the first recovery
    // (recovery_rise_n going 0->1) is rise #2, the next is #3, etc. — fixes
    // every recovery being announced as "Secondary rise" regardless of count.
    recovery_rise_n++;
    uint8_t rise_num = recovery_rise_n + 1;
    Serial.print(">> State: RISING (");
    Serial.print(rise_num);
    Serial.print(ordinalSuffix(rise_num));
    Serial.print(" rise — ");
    Serial.print(rise_mm);
    Serial.print(" mm exceeds confirmed peak ");
    Serial.print(peak_at_confirm);
    Serial.println(" mm)");
    sendTelegram("📈 " + String(rise_num) + String(ordinalSuffix(rise_num)) +
                 " rise detected\n"
                 "Now " + String(rise_mm) + " mm, past the earlier "
                 + String(peak_at_confirm) + " mm hold.\n"
                 "Still climbing — the cycle isn't over.");
    saveLiveState();
    return;
  }

  switch (state) {
    case ST_INITIAL:
      // Stage 1. Consecutive confirmations, so 3 mm of ToF noise can't do it.
      if (rise_mm >= rising_threshold_mm) {
        if (++rise_consec >= RISING_CONSEC) {
          state = ST_RISING;
          if (highest_state_this_cycle < ST_RISING) highest_state_this_cycle = ST_RISING;
          Serial.print(">> State: RISING (rise ");
          Serial.print(rise_mm); Serial.print(" mm x");
          Serial.print(RISING_CONSEC); Serial.println(" samples)");
          sendTelegram("📈 Starter is rising\n"
                       "Gained: " + String(rise_mm) + " mm\n"
                       "Elapsed: " + String(mins_elapsed) + " min");
          saveLiveState();
        }
      } else {
        rise_consec = 0;
      }
      break;

    case ST_RISING: {
      // Stage 3: peak candidate has stood still long enough, AND it is a
      // real peak (tall enough, and it was held that high for a while).
      bool tall_enough   = peak_rise_mm >= min_meaningful_rise_mm;
      bool sustained_ok  = sustained_active &&
                           (now - sustained_since_ms) / 60000 >= min_sustained_minutes;
      bool stable_ok     = (now - last_newmax_ms) / 60000 >= peak_stable_minutes;
      if (tall_enough && sustained_ok && stable_ok) {
        state = ST_PEAKED;
        peaked_time     = now;
        if (highest_state_this_cycle < ST_PEAKED) highest_state_this_cycle = ST_PEAKED;
        if (!first_peaked_time) first_peaked_time = now;   // v1.5: capture once
        peak_at_confirm = peak_rise_mm;
        falling_pending = false;
        Serial.print(">> State: PEAKED (holding ");
        Serial.print(peak_rise_mm);
        Serial.print(" mm for ");
        Serial.print((now - last_newmax_ms) / 60000);
        Serial.println(" min — soft, reversible)");
        // NOTE: no closeCycle() here. The cycle is only written once a fall is
        // confirmed, because the starter may still be climbing a staircase.
        sendTelegram("🎯 Starter has peaked\n"
                     "Peak rise: " + String(peak_rise_mm) + " mm\n"
                     "Time to peak: " + String(mins_elapsed) + " min\n"
                     + peakGuidance());
        saveLiveState();
      }
      break;
    }

    case ST_PEAKED: {
      // Stage 4: fall must be both deep enough AND sustained.
      int16_t thr = fallThresholdMm();
      if (rise_mm <= peak_rise_mm - thr) {
        if (!falling_pending) {
          falling_pending = true;
          fall_since_ms   = now;
          Serial.print(">> Fall candidate: down ");
          Serial.print(peak_rise_mm - rise_mm);
          Serial.print(" mm (threshold "); Serial.print(thr);
          Serial.println(" mm), confirming...");
        } else if ((now - fall_since_ms) / 60000 >= fall_stable_minutes) {
          state = ST_FALLING;
          if (highest_state_this_cycle < ST_FALLING) highest_state_this_cycle = ST_FALLING;
          Serial.print(">> State: FALLING (confirmed over ");
          Serial.print(fall_stable_minutes); Serial.println(" min)");
          // The peak is real and over: NOW record the cycle.
          closeCycle();
          backfillPeakHold();
          checkMaturity();
          checkWeakCycles();
          sendTelegram("📉 Past peak, falling\n"
                       "Down " + String(peak_rise_mm - rise_mm) + " mm from a "
                       + String(peak_rise_mm) + " mm peak.\n"
                       "Feed soon.");
          falling_pending = false;
          saveLiveState();
        }
      } else {
        falling_pending = false;   // came back up: not a fall after all
      }
      break;
    }

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
    <button id="feed" style="background:var(--warn);border-color:var(--warn)">🥄 Toggle feeding mode</button>
    <button id="csv">Download CSV</button>
    <button id="reset">Reset to idle</button>
    <button id="wipehist" style="background:var(--bad);border-color:var(--bad)">🗑️ Clear chart</button>
    <span id="msg"></span>
  </div>

  <!-- Pam v1.1a: starter age tracker -->
  <h2>Starter age</h2>
  <div class="row">
    <label for="sday" style="font-size:13px;color:var(--dim)">Day of starter (0 = disabled, auto-increments daily)</label>
    <input id="sday" type="number" min="0" max="999">
    <button id="savesday">Save</button>
    <span id="sdaymsg" style="font-size:13px;color:var(--dim)"></span>
  </div>
  <div id="sdayinfo" style="font-size:13px;color:var(--dim);margin-top:6px"></div>

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
      <th>Fed</th><th>Peak</th><th>Rise %</th><th>To peak</th><th>Rate</th><th>Temp</th><th></th></tr></thead><tbody></tbody></table>
    <div class="row">
      <button id="cyccsv">⬇️ Download results (CSV)</button>
      <button id="wipe">Wipe cycle history</button>
      <span id="wipemsg" style="font-size:13px;color:var(--dim)"></span>
    </div>
  </div>

  <div class="starter-only">
    <h2>Maturity trend</h2>
    <div id="trendCharts" style="display:flex;flex-wrap:wrap;gap:16px"></div>
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

    // Pam v1.1a: feeding mode button state and header hint
    if(now.feeding_mode){
      $('feed').textContent = '▶️ Exit feeding mode';
      $('feed').style.background = 'var(--ok)';
      $('feed').style.borderColor = 'var(--ok)';
      // Show a visible pause banner in the state area
      $('state').textContent = 'FEEDING (paused)';
      $('state').className = '';
      $('state').style.color = 'var(--warn)';
    } else {
      $('feed').textContent = '🥄 Toggle feeding mode';
      $('feed').style.background = 'var(--warn)';
      $('feed').style.borderColor = 'var(--warn)';
      $('state').style.color = '';
    }

    // Pam v1.1a: starter age display
    if(now.starter_day !== undefined){
      if(!$('sday').matches(':focus')) $('sday').value = now.starter_day;
      if(now.starter_day === 0){
        $('sdayinfo').textContent = 'Starter age tracking disabled (set to 1+ to enable)';
      } else {
        // Give phase guidance based on day
        let phase;
        if(now.starter_day <= 4) phase = 'bacterial phase (chaotic, dramatic rises expected)';
        else if(now.starter_day <= 7) phase = 'silent phase (quiet period, don\'t intervene)';
        else if(now.starter_day <= 12) phase = 'yeast establishing (real cycles emerging)';
        else if(now.starter_day <= 18) phase = 'approaching maturity';
        else phase = 'mature starter';
        $('sdayinfo').innerHTML = '<b>Day '+now.starter_day+'</b> &middot; '+phase;
      }
    }

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

let lastCycles=[];   // mirrored each poll so the download button needs no refetch
async function loadCycles(){
  try{
    const r=await fetch('/api/history_cycles',{cache:'no-store'});
    const j=await r.json();
    lastCycles=j.cycles||[];
    const tb=$('cyc').querySelector('tbody'); tb.innerHTML='';
    if(!j.cycles.length){
      tb.innerHTML='<tr><td colspan="7" style="color:var(--dim)">'+
        'No completed cycles yet. One gets saved each time the starter rises.</td></tr>';
      return;
    }
    const statusLabel=['initial','rising','peaked','falling'];
    const statusClass=['none','','','good'];
    j.cycles.slice().reverse().forEach(c=>{
      const sh=c.jar_height_mm-c.baseline_mm;
      const pct=sh>0? (100*c.peak_rise_mm/sh) : null;
      const rateMmHr=(c.time_to_peak_min>0)? (c.peak_rise_mm/(c.time_to_peak_min/60)) : null;
      const st=c.status||0;
      const tr=document.createElement('tr');
      tr.innerHTML =
        '<td>'+(c.timestamp? new Date(c.timestamp*1000)
                 .toLocaleString([], {month:'short',day:'numeric',
                                      hour:'2-digit',minute:'2-digit'}) : '&mdash;')+'</td>'+
        '<td>'+c.peak_rise_mm+' mm</td>'+
        '<td>'+(pct==null?'&mdash;':pct.toFixed(0)+'%')+'</td>'+
        '<td>'+(c.time_to_peak_min? fmtDur(c.time_to_peak_min):'&mdash;')+'</td>'+
        '<td>'+(rateMmHr==null?'&mdash;':rateMmHr.toFixed(1)+' mm/hr')+'</td>'+
        '<td>'+(c.avg_temp_c==null?'&mdash;':c.avg_temp_c.toFixed(1)+'\u00b0')+'</td>'+
        '<td><span class="tag '+statusClass[st]+'">'+statusLabel[st]+'</span></td>';
      tb.appendChild(tr);
    });
    renderTrendCharts(j.cycles);
  }catch(e){}
}

// Pam v1.5: three small trend sparklines (peak rise, time-to-peak, rise
// rate) across the last cycles, so maturity progress is visible at a
// glance. Independent of the main 24h chart — own section, own function,
// nothing here touches draw().
function renderTrendCharts(cycles){
  const box=$('trendCharts');
  if(!cycles.length){ box.innerHTML='<p style="color:var(--dim)">Not enough cycles yet.</p>'; return; }
  const ordered=cycles.slice(); // already oldest-first from the API
  const peakSeries=ordered.map(c=>c.peak_rise_mm);
  const peakedOnly=ordered.filter(c=>(c.status||0)>=2 && c.time_to_peak_min>0);
  const t2pSeries=peakedOnly.map(c=>c.time_to_peak_min/60); // hours
  const rateSeries=peakedOnly.map(c=>c.peak_rise_mm/(c.time_to_peak_min/60));

  function spark(title,unit,values){
    if(!values.length) return '<div style="flex:1;min-width:220px"><h3 style="margin:4px 0">'+title+'</h3>'+
      '<p style="color:var(--dim);font-size:13px">No peaked cycles yet.</p></div>';
    const w=260,h=90,pad=8;
    const min=Math.min(...values), max=Math.max(...values);
    const range=(max-min)||1;
    const stepX=values.length>1? (w-2*pad)/(values.length-1) : 0;
    const pts=values.map((v,i)=>{
      const x=pad+i*stepX;
      const y=h-pad-((v-min)/range)*(h-2*pad);
      return x.toFixed(1)+','+y.toFixed(1);
    }).join(' ');
    const dots=values.map((v,i)=>{
      const x=pad+i*stepX;
      const y=h-pad-((v-min)/range)*(h-2*pad);
      return '<circle cx="'+x.toFixed(1)+'" cy="'+y.toFixed(1)+'" r="2.5" fill="var(--accent,#4a9)"/>';
    }).join('');
    const last=values[values.length-1];
    return '<div style="flex:1;min-width:220px">'+
      '<h3 style="margin:4px 0">'+title+' <span style="font-weight:400;color:var(--dim);font-size:13px">('+last.toFixed(1)+unit+' latest)</span></h3>'+
      '<svg viewBox="0 0 '+w+' '+h+'" style="width:100%;height:90px">'+
      '<polyline points="'+pts+'" fill="none" stroke="var(--accent,#4a9)" stroke-width="2"/>'+
      dots+'</svg></div>';
  }

  box.innerHTML =
    spark('Peak rise', 'mm', peakSeries) +
    spark('Time to peak', 'h', t2pSeries) +
    spark('Rise rate', 'mm/hr', rateSeries);
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

// Pam v1.1a: dashboard feed button. Toggles feeding mode without needing
// to reach the physical GPIO 18 button inside the styrofoam box.
$('feed').onclick=async()=>{
  const inFeeding = now && now.feeding_mode;
  const confirmMsg = inFeeding
    ? 'Exit feeding mode? Will re-calibrate at current level.'
    : 'Enter feeding mode? Tracking paused until you press again.';
  if(!confirm(confirmMsg)) return;
  $('msg').textContent = inFeeding ? 'exiting feeding mode…' : 'entering feeding mode…';
  try{
    const r = await fetch('/api/feeding_toggle',{method:'POST'});
    const j = await r.json();
    $('msg').textContent = j.ok ? j.message : 'failed: '+(j.error||'');
  }catch(e){ $('msg').textContent='failed: no response'; }
  tick();
};

// Pam v1.2a: wipe the 24-hour chart buffer (useful after a disturbance)
$('wipehist').onclick=async()=>{
  if(!confirm('Clear all chart data (24-hour history)? Cycle history is NOT affected.')) return;
  try{
    const r = await fetch('/api/wipe_history',{method:'POST'});
    const j = await r.json();
    $('msg').textContent = j.ok ? 'chart cleared ('+j.cleared+' samples)' : 'failed';
  }catch(e){ $('msg').textContent='failed: no response'; }
  tick();
  draw();
};

// Pam v1.1a: save starter day
$('savesday').onclick=async()=>{
  const v = parseInt($('sday').value, 10);
  if(isNaN(v) || v < 0 || v > 999){
    $('sdaymsg').textContent = 'must be 0-999 (0 disables tracking)';
    return;
  }
  try{
    const r = await fetch('/api/starter_day?day='+v,{method:'POST'});
    const j = await r.json();
    if(j.ok){
      $('sdaymsg').textContent = v === 0
        ? 'tracking disabled'
        : 'set to Day '+v+' (auto-increments daily)';
    } else {
      $('sdaymsg').textContent = 'failed: '+(j.error||'');
    }
  }catch(e){ $('sdaymsg').textContent='failed: no response'; }
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

// v1.6: wrap up a test run (e.g. a discard trial) by exporting the
// completed-cycles table to CSV before wiping it, so finishing a test
// leaves a record instead of just disappearing when you start tracking the
// mother starter (or a new test) fresh.
$('cyccsv').onclick=()=>{
  if(!lastCycles.length){ $('wipemsg').textContent='no completed cycles to export yet'; return; }
  const statusLabel=['initial','rising','peaked','falling'];
  const rows=[['fed_at','baseline_mm','jar_height_mm','peak_rise_mm','rise_pct',
               'time_to_peak_min','rate_mm_per_hr','avg_temp_c','temp_min_c',
               'temp_max_c','peak_hold_min','status']];
  lastCycles.forEach(c=>{
    const sh=c.jar_height_mm-c.baseline_mm;
    const pct=sh>0? (100*c.peak_rise_mm/sh) : '';
    const rate=(c.time_to_peak_min>0)? (c.peak_rise_mm/(c.time_to_peak_min/60)) : '';
    rows.push([
      c.timestamp? new Date(c.timestamp*1000).toISOString() : '',
      c.baseline_mm, c.jar_height_mm, c.peak_rise_mm,
      pct===''?'':pct.toFixed(0), c.time_to_peak_min,
      rate===''?'':rate.toFixed(1),
      c.avg_temp_c==null?'':c.avg_temp_c.toFixed(1),
      c.temp_min_c==null?'':c.temp_min_c.toFixed(1),
      c.temp_max_c==null?'':c.temp_max_c.toFixed(1),
      c.peak_hold_min, statusLabel[c.status||0]
    ]);
  });
  const blob=new Blob([rows.map(r=>r.join(',')).join('\n')],{type:'text/csv'});
  const a=document.createElement('a');
  const stamp=new Date().toISOString().slice(0,16).replace(/[:T]/g,'-');
  a.href=URL.createObjectURL(blob); a.download='sourdough_cycles_'+stamp+'.csv'; a.click();
};

$('wipe').onclick=async()=>{
  if(!confirm('Erase all saved cycles? Download results first if you want to '+
              'keep them — wiping is how you start clean for the mother '+
              'starter (or a new test). Peak prediction and phase '+
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
  // Pam v1.1a: feeding mode status for dashboard button
  j += ",\"feeding_mode\":" + String(feeding_mode ? "true" : "false");
  // Pam v1.1a: starter age tracking (Day X)
  j += ",\"starter_day\":" + String(starter_day);
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
    j += ",\"status\":" + String(c.status);
    j += ",\"avg_temp_c\":" + (c.avg_temp_c10 == INT16_MIN ? String("null")
                                : String(c.avg_temp_c10 / 10.0f, 1));
    j += ",\"temp_min_c\":" + (c.temp_min_c10 == INT16_MIN ? String("null")
                                : String(c.temp_min_c10 / 10.0f, 1));
    j += ",\"temp_max_c\":" + (c.temp_max_c10 == INT16_MIN ? String("null")
                                : String(c.temp_max_c10 / 10.0f, 1));
    j += ",\"peak_hold_min\":" + String(c.peak_hold_min);
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
        if (ran_min >= 60) closeCycle();
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

// Pam v1.1a: dashboard feed button. Toggles feeding mode without needing
// to reach the physical GPIO 18 button, which is inside the styrofoam box.
// Same behavior as physical button: press once to pause, press again to
// exit and re-baseline.
void handleFeedingToggle() {
  if (feeding_mode) {
    exitFeedingMode(false);
    server.send(200, "application/json",
                "{\"ok\":true,\"feeding_mode\":false,\"message\":\"exited feeding mode, re-calibrated\"}");
  } else {
    enterFeedingMode();
    server.send(200, "application/json",
                "{\"ok\":true,\"feeding_mode\":true,\"message\":\"entered feeding mode\"}");
  }
}

// Pam v1.1a: set starter age (day X). Used to manually track starter maturity
// for phase classification. 0 = disable tracking, 1+ = Day N.
void handleStarterDay() {
  if (!server.hasArg("day")) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"missing day parameter\"}");
    return;
  }
  int day = server.arg("day").toInt();
  if (day < 0 || day > 999) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"day must be 0-999\"}");
    return;
  }
  starter_day = (uint16_t)day;
  prefs.putUShort("s_day", starter_day);
  // Reset the day check timer so we don't immediately increment
  if (starter_day > 0 && ntp_ok) {
    last_day_check_epoch = (uint32_t)time(nullptr);
    prefs.putULong("s_dayck", last_day_check_epoch);
  }
  Serial.print(">> Starter day set to: "); Serial.println(starter_day);
  server.send(200, "application/json",
              "{\"ok\":true,\"starter_day\":" + String(starter_day) + "}");
}

// Pam v1.2a: wipe the 24-hour ring buffer (chart data). Useful when a
// disturbance has poisoned the Y-axis and you want a clean chart.
// Does NOT touch cycle history or current state — just the chart.
void handleWipeHistory() {
  uint16_t had = hist_count;
  hist_count = 0;
  hist_head = 0;
  last_log_ms = 0;
  Serial.print(">> Chart history wiped ("); Serial.print(had);
  Serial.println(" samples)");
  server.send(200, "application/json",
              "{\"ok\":true,\"cleared\":" + String(had) + "}");
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
  Serial.println("\n=== Sourdough Sensor v1.6 ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // v1.1 hardware: feeding button and status LEDs (Section 2.3/6.0).
  pinMode(FEED_BTN_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(LED_BUILTIN_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(LED_BUILTIN_PIN, LOW);
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
  // v1.1: invalid-cycle marks live in their own key so the Cycle blob layout —
  // and therefore pam's existing saved history — is untouched by the upgrade.
  cycle_invalid_mask = prefs.getUShort("cyc_bad", 0);
  // Pam v1.1a: starter age tracking (Day X)
  starter_day = prefs.getUShort("s_day", 0);
  last_day_check_epoch = prefs.getULong("s_dayck", 0);
  // v1.1 tunables, all runtime-settable via /api/config and persisted.
  rising_threshold_mm    = (int16_t)prefs.getShort("t_rise",  rising_threshold_mm);
  min_meaningful_rise_mm = (int16_t)prefs.getShort("t_min",   min_meaningful_rise_mm);
  min_sustained_minutes  = prefs.getUShort("t_sust",  min_sustained_minutes);
  peak_stable_minutes    = prefs.getUShort("t_stable", peak_stable_minutes);
  peak_fall_pct          = prefs.getUShort("t_fallp", peak_fall_pct);
  fall_stable_minutes    = prefs.getUShort("t_fallm", fall_stable_minutes);
  secondary_rise_mm      = (int16_t)prefs.getShort("t_sec",   secondary_rise_mm);
  warmup_minutes         = prefs.getUShort("t_warm",  warmup_minutes);
  feed_interval_hours    = prefs.getUShort("t_feed",  feed_interval_hours);
  Serial.print("NVS: jar height "); Serial.print(jar_height_mm);
  Serial.print(" mm, "); Serial.print(cyc_count); Serial.println(" saved cycles");
  Serial.print("Peak detector: rise>="); Serial.print(rising_threshold_mm);
  Serial.print("mm x"); Serial.print(RISING_CONSEC);
  Serial.print(", meaningful>="); Serial.print(min_meaningful_rise_mm);
  Serial.print("mm held "); Serial.print(min_sustained_minutes);
  Serial.print("min, stable "); Serial.print(peak_stable_minutes);
  Serial.print("min, fall "); Serial.print(peak_fall_pct);
  Serial.print("%/"); Serial.print(fall_stable_minutes); Serial.println("min");

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
  server.on("/api/feeding_toggle", HTTP_POST, handleFeedingToggle);  // Pam v1.1a
  server.on("/api/starter_day", HTTP_POST, handleStarterDay);        // Pam v1.1a
  server.on("/api/wipe_history", HTTP_POST, handleWipeHistory);      // Pam v1.2a
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
  checkFeedButton();    // Pam v1.1a: bug fix — Buzz forgot to call this
  updateLeds();         // Pam v1.1a: bug fix — Buzz forgot to call this
  server.handleClient();

  // Pam v1.1a: check for day rollover once per minute (cheap check)
  static unsigned long last_day_check_ms = 0;
  if (starter_day > 0 && ntp_ok && millis() - last_day_check_ms > 60000) {
    last_day_check_ms = millis();
    uint32_t now_epoch = (uint32_t)time(nullptr);
    // 86400 seconds = 24 hours
    if (last_day_check_epoch > 0 && (now_epoch - last_day_check_epoch) >= 86400) {
      starter_day++;
      last_day_check_epoch = now_epoch;
      prefs.putUShort("s_day", starter_day);
      prefs.putULong("s_dayck", last_day_check_epoch);
      Serial.print(">> Starter day advanced to: "); Serial.println(starter_day);
    }
  }

  if (millis() - last_sample_ms < SAMPLE_INTERVAL_MS) return;
  last_sample_ms = millis();

  float temp = sht31_ok ? sht31.readTemperature() : NAN;
  float hum  = sht31_ok ? sht31.readHumidity()    : NAN;
  uint16_t dist = readDistance();

  int16_t rise_mm = 0;
  if (state != ST_IDLE && dist > 0) {
    rise_mm = (int16_t)baseline_dist - (int16_t)dist;
    // Pam v1.2a BUG FIX: don't update peak or run state machine during feeding
    // mode. Buzz's v1.1 forgot this — the jar being lifted registers as a
    // 100mm+ rise, poisoning peak_rise_mm and the chart.
    if (!feeding_mode) {
      if (rise_mm > peak_rise_mm) {
        peak_rise_mm = rise_mm;
        peak_time = millis();
      }
      updateState(rise_mm);
    }
  }

  // Cycle temperature stats — needed for feature 3's +/-3 C stability rule.
  // Pam v1.2a: skip during feeding mode (jar out of box means temp readings
  // are of kitchen air, not fermentation environment).
  if (state != ST_IDLE && !isnan(temp) && !feeding_mode) {
    temp_sum += temp; temp_n++;
    int16_t t10 = (int16_t)lroundf(temp * 10.0f);
    if (t10 < temp_min_c10) temp_min_c10 = t10;
    if (t10 > temp_max_c10) temp_max_c10 = t10;
  }

  last_temp = temp; last_hum = hum; last_dist = dist; last_rise = rise_mm;

  // Feed the 5-minute rate window every ~10 s, then evaluate the safety
  // alerts. Both only make sense once a baseline exists.
  // Pam v1.2a: skip during feeding mode (spike from opening would trigger
  // runaway alert and pollute rate calculations).
  if (state != ST_IDLE && dist > 0 && !feeding_mode &&
      millis() - last_rate_ms >= 10000) {
    last_rate_ms = millis();
    rate_buf[rate_head] = { millis(), rise_mm };
    rate_head = (rate_head + 1) % RATE_SLOTS;
    if (rate_n < RATE_SLOTS) rate_n++;
    rise_rate_x100 = computeRiseRateX100();
  }
  // Pam v1.2a: skip safety checks during feeding (would fire false alarms)
  if (!feeding_mode) {
    checkOverflow(dist, rise_mm);
    checkLevainTarget(rise_mm);
  }

  if (millis() - last_log_ms >= LOG_INTERVAL_MS || hist_count == 0) {
    last_log_ms = millis();
    // Pam v1.2a: during feeding mode, log placeholder distance so chart
    // doesn't spike from jar being lifted.
    // Pam v1.4: also freeze temp/hum to the snapshot taken at feeding entry,
    // so the chart doesn't spike when jar is out of the styrofoam.
    if (feeding_mode) {
      historyPush(millis() / 1000, baseline_dist, 0,
                  feeding_frozen_temp, feeding_frozen_hum, (uint8_t)state);
    } else {
      historyPush(millis() / 1000, dist, rise_mm, temp, hum, (uint8_t)state);
    }
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
