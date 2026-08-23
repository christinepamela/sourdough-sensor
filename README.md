# sourdough-sensor

ESP32 monitor for a sourdough starter jar: measures rise height, temperature and
humidity, detects the rise/peak/fall cycle, alerts via Telegram, and serves a
local web dashboard.

Project slug: `SOURDOUGH_SENSOR` — plan at `PLANS/SOURDOUGH_SENSOR_PLAN.md`.

## Layout

```
sourdough_monitor/            <- the live sketch (Arduino needs folder == .ino name)
  sourdough_monitor.ino
  sourdough_types.h           <- shared structs; see header for why they live here
  secrets.h.example           <- committed template
  secrets.h                   <- your real credentials, gitignored
archive/
  sourdough_v0_4.ino          <- pam's baseline, kept unmodified for reference
  sourdough_v0_5.ino          <- dashboard only
  sourdough_v0_8.ino          <- snapshot of the current version
```

## Hardware

| Part | Detail |
|---|---|
| MCU | ESP32 DevKitC-32 (WROOM-32, 38-pin), USB powered, always on |
| Temp/humidity | SHT31 over I²C @ `0x44` |
| Distance | VL53L0X time-of-flight over I²C @ `0x29`, under the jar lid |
| Calibrate button | Tactile on GPIO 19 → GND, `INPUT_PULLUP` |
| Feed button | Tactile on GPIO 18 → GND, `INPUT_PULLUP` *(v1.1)* |
| Status LED | GPIO 17 → 220 Ω → LED → GND *(v1.1)* |
| Heartbeat LED | Onboard GPIO 2, no wiring *(v1.1)* |
| I²C pins | SDA 21, SCL 22 (ESP32 defaults) |
| Jar | 750 ml Mason jar, ~175 mm interior height, sensor through the lid |

### LED patterns (v1.1)

GPIO 17 carries cycle state; GPIO 2 is a slow heartbeat so a hung sketch is
visible without opening the dashboard.

| Pattern | Meaning |
|---|---|
| Solid | Feeding pause, or peaked |
| Fast blink (250 ms) | Feeding pause about to time out |
| Very fast blink (120 ms) | Distance sensor lost, recovery running |
| Slow blink (1.5 s) | Rising |
| Medium blink (600 ms) | Warming up after calibration |
| Very slow blink (3 s) | Initial / falling |
| Off | Idle |

## First-time setup

```bash
cp sourdough_monitor/secrets.h.example sourdough_monitor/secrets.h
# edit secrets.h with your WiFi + Telegram details
```

Install the toolchain and libraries:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "UniversalTelegramBot" "Adafruit SHT31 Library" \
                        "Adafruit_VL53L0X" "ArduinoJson"
```

## Build and flash

Use the **huge_app** partition scheme. The default scheme leaves only 12 % of
flash free once the TLS stack and the embedded dashboard are in; `huge_app`
drops that to 36 % used and leaves room for the remaining features.

```bash
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=huge_app sourdough_monitor
arduino-cli upload  --fqbn esp32:esp32:esp32:PartitionScheme=huge_app -p COM5 sourdough_monitor
arduino-cli monitor -p COM5 -c baudrate=115200
```

In the Arduino IDE the same setting is **Tools → Partition Scheme → Huge APP
(3MB No OTA/1MB SPIFFS)**.

## Using it

1. Power on. Serial and a Telegram message both report the IP.
2. Open `http://sourdough.local/` (or the IP) on any device on the same WiFi.
3. Feed the starter, seat the lid, then press the button — or click
   **Set baseline** on the dashboard. That distance becomes the zero point.
4. Watch the rise. Telegram fires on RISING, PEAKED and FALLING.

## HTTP API

| Route | Method | Returns |
|---|---|---|
| `/` | GET | the dashboard (single page, no external assets) |
| `/api/now` | GET | live readings + device health as JSON |
| `/api/history` | GET | up to 24 h of samples, 1/min, as compact arrays |
| `/api/history_cycles` | GET | last 10 completed cycles, persisted in NVS |
| `/api/prediction` | GET | the whole inference: phase, comparable cycles, prediction |
| `/api/calibrate` | POST | sets the baseline; `{"ok":true,"baseline":mm}` |
| `/api/config?jar=<mm>` | POST | sets jar interior height (persisted) |
| `/api/config?mode=starter\|levain` | POST | switches mode; resets tracking to IDLE |
| `/api/config?levain_target=<pct>` | POST | sets the levain target %% (persisted) |
| `/api/config?feed_interval_h=<h>` | POST | feed-reminder interval, 8–48 h *(v1.1)* |
| `/api/reset` | POST | discards the in-progress cycle, back to IDLE |
| `/api/reset_cycles` | POST | wipes saved cycles + maturity latch (LAN-trusted, no auth) |
| `/api/cycle_delete?i=<n>` | POST | deletes one cycle by display index *(v1.1)* |
| `/api/cycle_flag?i=<n>[&valid=1]` | POST | exclude/restore one cycle *(v1.1)* |

`/api/history` rows are `[t_seconds, dist_mm, rise_mm, temp_c10, hum_pct, state]`.
`temp_c10` is temperature ×10 with `-32768` meaning "no reading"; `hum_pct` uses
`255` the same way; `state` indexes
`IDLE, INITIAL, RISING, PEAKED, FALLING, WARMUP, FEEDING`.

The two v1.1 states are appended **after** the v0.9 values on purpose:
renumbering would silently relabel every sample already saved in history.

`i` in the cycle endpoints is the **display index** returned as `i` by
`/api/history_cycles` (0 = oldest shown), not a ring offset. `cycle_flag`
keeps the cycle in history but excludes it from prediction and maturity;
`cycle_delete` removes it and compacts the ring.

## Version history

| Version | Change |
|---|---|
| v0.4 | Baseline from pam: sensors, button calibration, state machine, Telegram |
| v0.5 | Feature 1 — web dashboard, 24 h in-RAM history, mDNS, NTP, secrets split out |
| v0.6 | Feature 5 — NVS persistence, cycle history, true jar-geometry rise % |
| v0.7 | Feature 3 — time-to-peak prediction, phase classification, maturity alert |
| v0.8 | Feature 2 — headroom overflow alert + 5-min runaway-rate alert |
| v0.9 | Feature 4 — starter / levain modes, target line, `/api/reset_cycles` |
| v1.1 | Peak detection rebuilt on the staircase model; feeding button + LEDs; weak-cycle, temperature and feed-safety alerts; sensor auto-recovery; per-cycle delete / exclude |

## Hardware verification status

- **v0.4** — verified on hardware. Sensors + WiFi + Telegram + state machine all tested and working.
- **v0.5–v0.9** — compile-verified only when written; subsequently run on real hardware for 8 days (days 1–8 of the first starter attempt). That run is what produced the dataset v1.1 is tuned against, and what exposed the peak-detection failure.
- **v1.1** — compile-verified, and **validated by replaying all seven real CSVs from that 8-day run** through a harness implementing the same algorithm. Not yet run on hardware. The new GPIO 18 button and GPIO 17 LED have never been exercised in software.

### What "validated" means here, precisely

The replay harness (`replay3.py`, in the author's workspace) implements the
v1.1 detector in Python and runs it over the real day2–day8 CSVs. A companion
check (`verify_constants.py`) asserts that every threshold in the harness
equals the constant compiled into the sketch, so the results below describe
the shipped code rather than a drifted copy.

| File | Expected | Result |
|---|---|---|
| `day5_overnight_day6_10am` | true peak ~29 mm, not 3 mm | peak **29 mm**, 5 stair-steps, no false peak |
| `day6_1030pm` | doubling with a long plateau | peak **12 mm**, 5 h plateau survived |
| `day6_1130pm_deadsensor` | detect + recover | sensor-lost fires, recovery ladder runs |
| `day7_slow_cycle` | cold warning, no false peak | warns at t=1.47 h, no false peak |
| `day8_rescue_failed` | flagged weak, no false peak | weak, no false peak |
| `day2_full_cycle` | bacterial noise, no false peak | 2 stair-steps to 17 mm, no false peak |

This is **algorithm validation, not hardware validation.** It proves the
detector interprets real curves correctly. It says nothing about I²C
behaviour, button debounce, LED wiring, or Telegram delivery.

## Alerts

| Trigger | Message |
|---|---|
| Rise ≥ 6 mm for 3 consecutive samples | 📈 rising *(v1.1: was 3 mm, one sample)* |
| Peak confirmed (see staircase model) | 🎯 peaked — text now scales to starter maturity *(v1.1)* |
| Sustained fall below the dynamic threshold | 📉 falling *(v1.1: 15 % of peak, clamped 3–15 mm)* |
| Rise past a confirmed peak | 📈 secondary rise — the stair-step *(v1.1)* |
| Headroom ≤ 20 mm | 🚨 overflow warning — re-arms above 30 mm |
| Slope ≥ 3 mm/min over 5 min | ⚡ runaway fermentation — re-arms below 1.5 mm/min |
| 30 min before predicted peak | ⏰ peak expected — once per cycle |
| 3 comparable cycles all doubled | 🎉 mature starter — once per starter, latched in NVS |
| Levain target reached | 🎯 `Target reached (X%) at Y min — use now` — once per calibration |
| Weak or declining cycle | 🔍 weak cycle + intervention suggestions *(v1.1)* |
| < 24 °C for 60 min | ❄️ / 🥶 cool or stalled fermentation *(v1.1, once per cycle)* |
| > 30 °C for 30 min | 🔥 over-fermentation risk *(v1.1, once per cycle)* |
| Feed interval elapsed (default 24 h) | 🍽️ feed reminder — fires even if nothing rose *(v1.1)* |
| Distance sensor lost / recovered | ⚠️ / ✅ sensor status *(v1.1)* |
| Feeding pause entered / timed out | ⏸️ / ⏱️ *(v1.1)* |

## Peak detection: the staircase model (v1.1)

v0.9 declared a peak whenever no new maximum appeared for 10 minutes. Real
starters stall for hours mid-climb, so it fired at 3 mm, entered FALLING, and
had no way back — one run sat in FALLING for 22 hours while the jar climbed to
27 mm.

The data says these curves are **staircases, not bell curves**: `day5` stalls
for 258 minutes at ~10 mm before climbing to a true 29 mm peak. Any
"no new max for N minutes" rule either fires mid-rise or reports every peak
hours late.

So: **a stall is not evidence of a peak — only a sustained fall is.**

- `PEAKED` is **soft and reversible**. It alerts, but does not record.
- A rise past the confirmed peak returns to `RISING` (the stair-step).
- The cycle is recorded only once a fall is **confirmed** — 15 % of peak
  height (clamped 3–15 mm), sustained for 20 minutes.
- `FALLING` still watches for a late climb, which is the v0.9 dead end.

The brief's proposed slope gate (< 0.3 mm/h) was dropped: measured on real
data, the slope during `day5`'s pre-peak stall (median +1.42 mm/h) overlaps
the slope at its true plateau (median +0.48, p90 +1.27). The two states are
not separable by slope, so the gate adds false confidence, not information.

Full rationale is in the `THE STAIRCASE MODEL` comment above `updateState()`.

## Modes

| | Starter mode (default) | Levain / dough mode |
|---|---|---|
| Tracks | full rise / peak / fall cycle | rise to a target %% |
| Alerts | rising, peaked, falling | target reached, then peaked |
| Cycle history | recorded, trains prediction | **not** recorded |
| Dashboard | prediction, phase, cycle table | rise, chart, target line |

Levain builds are deliberately excluded from cycle history: different flour,
hydration and quantity from the maintenance starter, so folding them into the
same history would poison the prediction model with cycles that are not
comparable.

Switching mode resets tracking to IDLE and requires re-calibration. The target
percentage is only editable while in levain mode.

## Feeding pause (v1.1)

Taking the jar out to feed it produces a 5–30 mm swing that looks exactly like
fermentation. Press the **GPIO 18** button first and tracking freezes: no state
changes, no alerts, samples still logged but kept out of the peak, the
temperature statistics and the rate window.

Press again when you're done and it re-calibrates at the new level. It
auto-resumes after 20 minutes, with the LED blinking for the last two.

## Tuning

Thresholds live in three blocks at the top of the sketch: `PEAK DETECTION`,
`TEMPERATURE WARNINGS` / `WEAK CYCLE / FEED`, and `PREDICTION TUNING`.

They are **not** all the same quality of evidence, and it matters which is
which:

- **Peak-detection and temperature constants are measured.** Every one was
  derived from the day2–day8 CSVs and is checked against the replay harness.
- **Prediction and phase constants are still first-guess heuristics.** Nothing
  has yet produced three comparable cycles, so the phase classifier remains
  the least trustworthy code here — a hypothesis with a UI.

## Known limits

- **v1.1 has not run on hardware.** Algorithm-validated against real data
  (see above), which is a different and weaker claim.
- **The GPIO 18 button and GPIO 17 LED have never been exercised.** The wiring
  is in place but no software has driven it. Debounce is 200 ms, matching the
  existing button.
- **Prediction needs 3+ comparable cycles** at a similar temperature before it
  says anything. Expect roughly a week of feeding before it's useful.
- **Phase thresholds are guesses.** "Bacterial" in particular (fast peak +
  short hold) is a plausible reading of the literature, not something this code
  has ever observed.
- **The runaway threshold has never been exceeded by real data.** The fastest
  genuine 5-minute slope on record is 1.40 mm/min against a 3.00 mm/min
  trigger, so that alert is effectively untested.
- **Close-range ToF noise is the main hardware risk.** The VL53L0X degrades
  near the lid and the overflow alert lives at 20 mm. If it cries wolf, the fix
  is requiring N consecutive sub-threshold readings rather than one.
- **`rise_pct` denominator depends on `jar_height_mm` being right.** Measure
  sensor-face to jar-bottom and set it on the dashboard, or the percentage is
  confidently wrong.
- **No auth on the dashboard.** Anyone on your LAN can reset the baseline.
- **NVS wear:** state saves on every transition plus every 10 min while a cycle
  runs — roughly 150 writes/day worst case, far inside NVS endurance.
