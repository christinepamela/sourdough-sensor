// Sourdough Sensor v0.4 — baseline as supplied by pam, 2026-08-06.
// Archived unmodified for reference. Do not edit; work happens in
// ../sourdough_monitor/sourdough_monitor.ino
//
// NOTE: secrets below were redacted by pam in the original paste.

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include "Adafruit_VL53L0X.h"

// ====== FILL THESE IN ======
const char* WIFI_SSID     = "Cronus";
const char* WIFI_PASSWORD = "REDACTED";
const char* BOT_TOKEN     = "REDACTED";
const char* CHAT_ID       = "8982793170";
// ===========================

const int BUTTON_PIN = 19;

Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_VL53L0X vl53 = Adafruit_VL53L0X();
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

enum State { ST_IDLE, ST_INITIAL, ST_RISING, ST_PEAKED, ST_FALLING };
State state = ST_IDLE;
const char* stateNames[] = {"IDLE", "INITIAL", "RISING", "PEAKED", "FALLING"};

bool sht31_ok = false, vl53_ok = false, wifi_ok = false;
uint16_t baseline_dist = 0;
unsigned long baseline_time = 0;
int16_t peak_rise_mm = 0;
unsigned long peak_time = 0;

const int16_t RISING_THRESHOLD_MM = 3;
const int16_t PEAK_PLATEAU_MINUTES = 10;
const int16_t FALL_FROM_PEAK_MM = 5;

const int SMOOTH_N = 5;
uint16_t dist_buffer[SMOOTH_N] = {0};
int dist_idx = 0;
bool buffer_full = false;

bool last_button = HIGH;
unsigned long last_button_change = 0;

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
    sendTelegram("Calibration failed - starter too far from sensor.");
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
  String msg = "Starter calibrated\n";
  msg += "Baseline: " + String(d) + " mm";
  if (!isnan(temp)) msg += "\nTemp: " + String(temp, 1) + "C";
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
        sendTelegram("Starter is rising\nGained: " + String(rise_mm) + " mm\nElapsed: " + String(mins_elapsed) + " min");
      }
      break;
    case ST_RISING:
      if (mins_since_peak >= PEAK_PLATEAU_MINUTES && peak_rise_mm > RISING_THRESHOLD_MM) {
        state = ST_PEAKED;
        Serial.println(">> State: PEAKED");
        sendTelegram("Starter has peaked!\nPeak rise: " + String(peak_rise_mm) + " mm\nTime to peak: " + String(mins_elapsed) + " min");
      }
      break;
    case ST_PEAKED:
      if (peak_rise_mm - rise_mm >= FALL_FROM_PEAK_MM) {
        state = ST_FALLING;
        Serial.println(">> State: FALLING");
        sendTelegram("Past peak, falling\nDown " + String(peak_rise_mm - rise_mm) + " mm from peak.");
      }
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Sourdough Sensor v0.4 ===");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Wire.begin();

  if (sht31.begin(0x44)) { sht31_ok = true; Serial.println("SHT31: OK"); }
  else Serial.println("SHT31: NOT FOUND");

  if (vl53.begin()) { vl53_ok = true; Serial.println("VL53L0X: OK"); }
  else Serial.println("VL53L0X: NOT FOUND");

  Serial.print("WiFi connecting to ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifi_ok = true;
    Serial.print("\nWiFi: OK, IP=");
    Serial.println(WiFi.localIP());
    secured_client.setInsecure();
    sendTelegram("Sourdough monitor online\nIP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi: FAILED (continuing without Telegram)");
  }

  Serial.println("\nt(s)\telapsed\tstate\tT\tH\tdist\trise\tpeak");
}

void loop() {
  checkButton();

  static unsigned long last_print = 0;
  if (millis() - last_print < 2000) return;
  last_print = millis();

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
