/*
  Shared types for the sourdough monitor.

  WHY THIS FILE EXISTS
  --------------------
  The Arduino build auto-generates function prototypes and injects them
  immediately after the last #include in the .ino. Any function whose
  signature mentions a type declared further down the .ino (`const Cycle&`,
  a `Phase` return) therefore gets a prototype referring to a type that does
  not exist yet, and the build fails with "'Cycle' does not name a type".

  Putting the types in a header that the .ino includes means they are known
  before the generated prototypes appear. This is the standard fix, not a
  workaround.
*/
#pragma once
#include <Arduino.h>

// One sample in the 24 h rolling chart buffer. 12 bytes packed.
struct Sample {
  uint32_t t;         // seconds since boot
  int16_t  dist;      // mm, 0 = out of range
  int16_t  rise;      // mm above baseline
  int16_t  temp_c10;  // temperature x10, INT16_MIN = no reading
  uint8_t  hum;       // %RH, 255 = no reading
  uint8_t  state;     // index into stateNames[]
};

// Cycle.flags bits (v1.1).
//   CYC_INVALID    — user marked it bad, or it was auto-flagged as implausible.
//                    Kept in history but excluded from prediction and maturity.
//   CYC_AUTOFLAG   — *we* raised the suspicion, not the user. Lets the
//                    dashboard say "looks unusual" rather than "you rejected
//                    this", and lets the user clear it without ambiguity.
const uint8_t CYC_INVALID  = 0x01;
const uint8_t CYC_AUTOFLAG = 0x02;

// One completed feed-to-peak cycle, persisted to NVS.
//
// v1.1 NVS COMPATIBILITY NOTE
// ---------------------------
// `flags` was added in v1.1. It lands in what was previously a tail padding
// byte: the struct is 4-byte aligned because of `timestamp`, so v0.9's 22
// bytes of members already occupied 24 bytes, and 23 bytes still do. sizeof
// is therefore UNCHANGED, the saved blob still passes loadCycles()' size
// check, and pam's existing cycle history survives the upgrade instead of
// being silently wiped. The static_assert in the .ino enforces this; if a
// future field breaks 24 bytes, that assert fires at compile time rather than
// eating someone's history at boot. loadCycles() masks the byte on read
// because a padding byte's value was never formally guaranteed.
struct Cycle {
  uint32_t timestamp;         // epoch of the feed, 0 if NTP was down
  uint16_t baseline_mm;       // lid-to-surface at feed time
  uint16_t jar_height_mm;     // geometry in force for this cycle
  int16_t  peak_rise_mm;
  uint16_t time_to_peak_min;  // 0 == never reached PEAKED (a "silent" cycle)
  int16_t  avg_temp_c10;      // INT16_MIN = no temp data
  int16_t  temp_min_c10;
  int16_t  temp_max_c10;
  int16_t  baseline_temp_c10; // temp at feed time — the "+/-3 C of baseline"
                              // rule is relative to THIS, not the mean
  uint16_t peak_hold_min;     // PEAKED -> FALLING duration, 0 if still held
  uint8_t  flags;             // v1.1: CYC_INVALID | CYC_AUTOFLAG (see above)
};

// The in-progress cycle as written to NVS, so a power cut doesn't lose it.
// millis() is meaningless across a reboot, so elapsed time is anchored to
// wall-clock epoch when NTP is up and to saved minute counts when it isn't.
struct LiveState {
  uint8_t  state;
  uint16_t baseline_dist;
  uint32_t baseline_epoch;
  uint32_t elapsed_min;       // fallback anchor when epoch == 0
  int16_t  peak_rise_mm;
  uint32_t mins_since_peak;   // fallback anchor
  uint32_t peak_epoch;
  float    temp_sum;
  uint32_t temp_n;
  int16_t  temp_min_c10;
  int16_t  temp_max_c10;
  uint8_t  cycle_recorded;
  uint32_t saved_epoch;       // when this snapshot was written
  int16_t  baseline_temp_c10;
  uint32_t mins_since_peaked; // how long PEAKED has been held
  uint8_t  prepeak_alert_sent;// don't re-alert after a reboot
  uint8_t  levain_target_hit; // don't re-alert the levain target after a reboot
  uint16_t levain_hit_min;    // minutes it took, for the dashboard summary
};

// Starter development stage, per pam's spec.
enum Phase { PH_UNKNOWN, PH_SILENT, PH_BACTERIAL, PH_YEAST, PH_MATURE };

// What the jar is doing right now.
//   MODE_STARTER — the maintenance starter. Track the full rise/peak/fall
//                  cycle; these cycles train the prediction model.
//   MODE_LEVAIN  — a build for a specific bake. Alert at a target % rise.
//                  Deliberately NOT recorded into cycle history: different
//                  flour, hydration and quantity would poison the model.
enum Mode { MODE_STARTER, MODE_LEVAIN };
