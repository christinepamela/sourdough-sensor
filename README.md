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
| Button | Tactile on GPIO 19, `INPUT_PULLUP` |
| I²C pins | SDA 21, SCL 22 (ESP32 defaults) |
| Jar | Mason jar, 14 cm interior height, sensor through the lid |

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
| `/api/reset` | POST | discards the in-progress cycle, back to IDLE |

`/api/history` rows are `[t_seconds, dist_mm, rise_mm, temp_c10, hum_pct, state]`.
`temp_c10` is temperature ×10 with `-32768` meaning "no reading"; `hum_pct` uses
`255` the same way; `state` indexes `IDLE, INITIAL, RISING, PEAKED, FALLING`.

## Version history

| Version | Change |
|---|---|
| v0.4 | Baseline from pam: sensors, button calibration, state machine, Telegram |
| v0.5 | Feature 1 — web dashboard, 24 h in-RAM history, mDNS, NTP, secrets split out |
| v0.6 | Feature 5 — NVS persistence, cycle history, true jar-geometry rise % |
| v0.7 | Feature 3 — time-to-peak prediction, phase classification, maturity alert |
| v0.8 | Feature 2 — headroom overflow alert + 5-min runaway-rate alert |

## Alerts

| Trigger | Message |
|---|---|
| Rise ≥ 3 mm | 📈 rising *(v0.4, unchanged)* |
| Peak plateau 10 min | 🎯 peaked *(v0.4, unchanged)* |
| 5 mm below peak | 📉 falling *(v0.4, unchanged)* |
| Headroom ≤ 20 mm | 🚨 overflow warning — re-arms above 30 mm |
| Slope ≥ 3 mm/min over 5 min | ⚡ runaway fermentation — re-arms below 1.5 mm/min |
| 30 min before predicted peak | ⏰ peak expected — once per cycle |
| 3 comparable cycles all doubled | 🎉 mature starter — once per starter, latched in NVS |

## Tuning

The `PREDICTION TUNING` and `OVERFLOW / SAFETY` blocks at the top of the sketch
hold every threshold. **They are first-guess heuristics, not measured values** —
nothing here has seen a real starter yet. Expect to move them once cycle data
exists.

## Known limits

- **Nothing has run on hardware.** Compile-verified only.
- **Prediction needs 3+ comparable cycles** at a similar temperature before it
  says anything. Expect roughly a week of feeding before it's useful.
- **Phase thresholds are guesses.** "Bacterial" in particular (fast peak +
  short hold) is a plausible reading of the literature, not something this code
  has ever observed.
- **`rise_pct` denominator depends on `jar_height_mm` being right.** Measure
  sensor-face to jar-bottom and set it on the dashboard, or the percentage is
  confidently wrong.
- **No auth on the dashboard.** Anyone on your LAN can reset the baseline.
- **NVS wear:** state saves on every transition plus every 10 min while a cycle
  runs — roughly 150 writes/day worst case, far inside NVS endurance.
