#include <Arduino.h>
#include <Wire.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SSD1306.h>
#include <SparkFun_Qwiic_Scale_NAU7802_Arduino_Library.h>
#include <esp_timer.h>
#include <atomic>
#include <math.h>

namespace {
constexpr int BUTTON = 0;
constexpr uint32_t MAX_US = 180000000;
constexpr float CAL_G = 629.0f;
NAU7802 scale;
Adafruit_SSD1306 oled(128, 64, &Wire, -1, 100000, 100000);
Adafruit_NeoPixel pixel(1, 39, NEO_GRB + NEO_KHZ800);
Preferences prefs;
struct Calibration { int32_t zero; float factor; };
bool measuringCalibration = false;
int64_t calibrationSum = 0;
unsigned calibrationSamples = 0;
uint32_t calibrationStarted = 0;
enum State { READY, CAL_EMPTY, CAL_WEIGHT, RECORDING, SAVING, POST, FAULT };
State state = READY;
struct Sample { uint32_t us; int32_t raw; }; // us=UINT32_MAX is end-of-run marker
QueueHandle_t queue;
std::atomic<bool> writerDone{false}, writeError{false};
File file;
float countsPerGram = 0;
int32_t zero = 0, calibrationZero = 0, latest = 0;
uint64_t startUs = 0, lastSampleUs = 0;
uint32_t samples = 0, dropped = 0, gaps = 0, clipped = 0;
char filename[32];
const char *faultReason = "Unknown";
bool sdOK = false, oledOK = false, sensorOK = false, haveSample = false;

size_t oledPosition = 1024;
void screen(const char *a, const char *b = "", const char *c = "", const char *d = "", bool incremental = false) {
  if (!oledOK) return;
  oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0,0); oled.println(a); oled.println(); oled.println(b);
  oled.println(c); oled.println(d);
  oledPosition = incremental ? 0 : 1024;
  if (!incremental) oled.display();
}
// Send at most 16 framebuffer bytes per loop (~2 ms at 100 kHz).
// Full READY refreshes must not hide short physical button presses.
void updateReadyDisplay() {
  if (!oledOK || state != READY || oledPosition >= 1024) return;
  if (oledPosition == 0) {
    const uint8_t address[] = {0x00, SSD1306_PAGEADDR, 0, 7, SSD1306_COLUMNADDR, 0, 127};
    Wire.beginTransmission(0x3c); Wire.write(address, sizeof(address));
    if (Wire.endTransmission() != 0) return;
  }
  Wire.beginTransmission(0x3c); Wire.write(uint8_t(0x40));
  Wire.write(oled.getBuffer() + oledPosition, 16);
  if (Wire.endTransmission() == 0) oledPosition += 16;
}
void color(uint8_t r, uint8_t g, uint8_t b) {
  pixel.setPixelColor(0, pixel.Color(r,g,b)); pixel.show();
}
void fault(const char *why) {
  faultReason = why; state = FAULT; color(40,0,40); screen("ERROR", why, "Reset to retry"); Serial.println(why);
}
// Exact I2C transaction lengths are checked: a failed read is never logged as thrust.
bool readSample(int32_t &raw) {
  Wire.beginTransmission(0x2a); Wire.write(0x12);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(uint8_t(0x2a), uint8_t(3)) != 3) return false;
  uint32_t v = uint32_t(Wire.read()) << 16;
  v |= uint32_t(Wire.read()) << 8; v |= Wire.read();
  raw = (v & 0x800000) ? int32_t(v | 0xff000000) : int32_t(v);
  return true;
}
bool average(int32_t &out) {
  int64_t sum = 0; unsigned n = 0; uint32_t began = millis();
  while (n < 160 && millis() - began < 3000) {
    if (scale.available()) { int32_t v; if (readSample(v)) { sum += v; ++n; } }
    delay(1);
  }
  if (n != 160) return false;
  out = int32_t(sum / n); return true;
}
// Only this task accesses the open file. Queue holds ~6.4 seconds at 320 SPS.
void writer(void *) {
  char block[4096]; size_t used = 0;
  auto flush = [&]() {
    if (used && file.write((const uint8_t *)block, used) != used) writeError = true;
    used = 0;
  };
  for (;;) {
    Sample s; xQueueReceive(queue, &s, portMAX_DELAY);
    if (s.us == UINT32_MAX) {
      flush();
      if (file.getWriteError()) writeError = true;
      file.flush();
      if (file.getWriteError()) writeError = true;
      file.close(); writerDone.store(true, std::memory_order_release); continue;
    }
    if (writeError) continue; // Drain until end marker even after disk failure.
    float g = (double(s.raw) - zero) / countsPerGram;
    char line[128];
    int n = snprintf(line, sizeof(line), "%lu.%03lu,%ld,%.3f,%.6f\n",
      (unsigned long)(s.us/1000), (unsigned long)(s.us%1000), (long)s.raw, g, g*0.00980665f);
    if (used + n > sizeof(block)) flush();
    memcpy(block + used, line, n); used += n;
  }
}
void stop() {
  // Sampling ends before any display or potentially blocking queue operation.
  state = SAVING;
  Sample end{UINT32_MAX,0}; xQueueSend(queue, &end, portMAX_DELAY);
  screen("SAVING", "Please wait...");
}
void beginRun() {
  Serial.println("RECORD requested: taring motor");
  if (!isfinite(countsPerGram) || fabsf(countsPerGram) < 0.001f) {
    screen("CALIBRATION NEEDED", "Hold button 2 sec"); return;
  }
  screen("TARING MOTOR", "Keep stand still");
  if (!average(zero)) { fault("Tare sensor timeout"); return; }
  unsigned id;
  for (id = 1; id < 100000; ++id) {
    snprintf(filename, sizeof(filename), "/THR%05u.csv", id);
    if (!SD_MMC.exists(filename)) break;
  }
  if (id == 100000) { fault("File names exhausted"); return; }
  file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) { fault("Cannot create SD file"); return; }
  file.printf("# sample_rate_sps=320,calibration_g=629,counts_per_g=%.9g,tare_raw=%ld\n", countsPerGram, (long)zero);
  file.println("elapsed_ms,raw_adc,grams,thrust_N"); file.flush();
  if (file.getWriteError()) { file.close(); fault("SD header failed"); return; }
  xQueueReset(queue); samples = dropped = gaps = clipped = 0;
  writeError = false; writerDone = false;
  screen("RECORDING / 320 SPS", "Max 180 seconds", "Press to stop", filename);
  color(40,0,0);
  // Discard conversion accumulated during SD setup / display transfer.
  if (scale.available()) { int32_t discard; readSample(discard); }
  startUs = lastSampleUs = esp_timer_get_time(); state = RECORDING;
  Serial.printf("RECORD started file=%s tare_raw=%ld\n", filename, (long)zero);
}
void shortPress() {
  switch (state) {
    case READY: beginRun(); break;
    case RECORDING: stop(); break;
    case POST: state = READY; color(0,0,40); break;
    case CAL_EMPTY:
    case CAL_WEIGHT:
      if (!measuringCalibration) {
        measuringCalibration = true; calibrationSum = 0; calibrationSamples = 0;
        calibrationStarted = millis();
        screen("CALIBRATION", state == CAL_EMPTY ? "Measuring empty..." : "Measuring 629 g...", "Keep load still");
        Serial.printf("CAL measuring stage=%u\n", unsigned(state));
      }
      break;
    default: break;
  }
}
void completeCalibrationMeasurement() {
  measuringCalibration = false;
  int32_t measured = int32_t(calibrationSum / calibrationSamples);
  if (state == CAL_EMPTY) {
    calibrationZero = measured; state = CAL_WEIGHT;
    Serial.printf("CAL empty_raw=%ld\n", (long)measured);
    screen("CALIBRATION 2/2", "Place 629 g", "Wait until stable", "Press AND release");
    return;
  }
  float factor = (double(measured)-calibrationZero)/CAL_G;
  Serial.printf("CAL loaded_raw=%ld delta=%ld counts_per_g=%.9g\n",
    (long)measured, (long)(measured-calibrationZero), factor);
  if (!isfinite(factor) || fabsf(factor) < 1) {
    screen("CALIBRATION 2/2", "No useful load change", "Apply 629 g to cell", "Press/release to retry"); return;
  }
  Calibration cal{calibrationZero, factor}, verify{};
  if (prefs.putBytes("cal", &cal, sizeof(cal)) != sizeof(cal) ||
      prefs.getBytes("cal", &verify, sizeof(verify)) != sizeof(verify) ||
      verify.zero != cal.zero || verify.factor != cal.factor) {
    screen("CALIBRATION 2/2", "Save failed", "Check flash storage", "Press/release to retry"); return;
  }
  zero = cal.zero; countsPerGram = cal.factor;
  state = READY; color(0,0,40);
  Serial.printf("CAL saved+verified empty_raw=%ld counts_per_g=%.9g\n", (long)zero, countsPerGram);
  screen("CALIBRATION SAVED", "Returning to READY");
}
}
void setup() {
  Serial.begin(115200); pinMode(BUTTON, INPUT_PULLUP); pixel.begin(); color(0,0,40);
  Wire.begin(17,18); Wire.setTimeOut(50);
  oledOK = oled.begin(SSD1306_SWITCHCAPVCC, 0x3c);
  if (!oledOK) { fault("OLED not found"); return; }
  screen("THRUST STAND", "Starting...");
  sensorOK = scale.begin() && scale.setSampleRate(NAU7802_SPS_320) && scale.calibrateAFE();
  Wire.setClock(100000);
  if (!sensorOK) { fault("NAU7802 not ready"); return; }
  if (!SD_MMC.setPins(36,37,38,34,33,35)) { fault("SD pin setup failed"); return; }
  // Conservative clock; retries handle card/socket startup settling.
  // External CMD/DAT pull-ups are still required by the SD interface.
  for (int pin : {37,38,34,33,35}) pinMode(pin, INPUT_PULLUP);
  delay(500);
  for (unsigned attempt = 0; attempt < 3 && !sdOK; ++attempt) {
    sdOK = SD_MMC.begin("/sdcard", false, false, 10000);
    if (!sdOK) { SD_MMC.end(); delay(500); }
  }
  if (!sdOK) { fault("4-bit SD mount failed"); return; }
  if (!prefs.begin("thrust", false)) { fault("NVS unavailable"); return; }
  Calibration cal{};
  if (prefs.getBytesLength("cal") == sizeof(cal) && prefs.getBytes("cal", &cal, sizeof(cal)) == sizeof(cal)) {
    if (isfinite(cal.factor) && fabsf(cal.factor) >= 1) { zero = cal.zero; countsPerGram = cal.factor; Serial.printf("CAL loaded empty_raw=%ld counts_per_g=%.9g\n", (long)zero, countsPerGram); }
  }
  queue = xQueueCreate(2048, sizeof(Sample));
  if (!queue || xTaskCreatePinnedToCore(writer,"SD writer",6144,nullptr,1,nullptr,0) != pdPASS) {
    fault("Cannot allocate logger"); return;
  }
  lastSampleUs = esp_timer_get_time();
  Serial.printf("BOOT OK sensor=NAU7802 rate=320 oled=SSD1306 sd=4bit card_MB=%llu calibrated=%u\n",
    SD_MMC.cardSize()/1048576, unsigned(countsPerGram != 0));
}
void loop() {
  if (state == FAULT) { if (Serial.available() && Serial.read() == '?') { Serial.printf("FAULT %s button=%u\n", faultReason, unsigned(digitalRead(BUTTON))); } delay(20); return; }
  // Diagnostics are serviced only outside acquisition/saving.
  if (state != RECORDING && state != SAVING && Serial.available()) {
    char command = Serial.read();
    if (command == '?') {
      Serial.printf("STATUS state=%u calibrated=%u button=%u raw=%ld grams=%.3f counts_per_g=%.9g sd_MB=%llu\n",
        unsigned(state), unsigned(countsPerGram != 0), unsigned(digitalRead(BUTTON)), (long)latest,
        countsPerGram ? (double(latest)-zero)/countsPerGram : 0.0, countsPerGram, SD_MMC.cardSize()/1048576);
    }
    if (command == 'r' && filename[0]) {
      File readback = SD_MMC.open(filename, FILE_READ);
      Serial.println("CSV_BEGIN");
      if (readback) { uint8_t data[512]; while (readback.available()) {
        size_t n = readback.read(data, sizeof(data)); if (!n) break; Serial.write(data, n);
      } readback.close(); }
      else Serial.println("READBACK_ERROR");
      Serial.println("CSV_END");
    }
  }
  uint64_t now = esp_timer_get_time();
  if (state == RECORDING && now-startUs >= MAX_US) stop();
  if (state != SAVING && scale.available()) {
    int32_t raw;
    if (!readSample(raw)) {
      if (state == RECORDING) { writeError = true; stop(); }
      else {
        static unsigned readFailures = 0;
        ++readFailures;
        Serial.printf("I2C read retry count=%u\n", readFailures);
        if (readFailures % 10 == 0) screen("SENSOR UNSTABLE", "Check I2C wiring");
        delay(2);
      }
    } else {
    now = esp_timer_get_time(); latest = raw; haveSample = true;
    if (state == RECORDING && now-startUs < MAX_US) {
      if (now-lastSampleUs > 5000) ++gaps;
      if (raw <= -8388600 || raw >= 8388600) ++clipped;
      Sample s{uint32_t(now-startUs), raw};
      if (xQueueSend(queue, &s, 0) != pdTRUE) ++dropped;
      ++samples;
    }
    lastSampleUs = now;
    if (measuringCalibration) {
      calibrationSum += raw; ++calibrationSamples;
      if (calibrationSamples >= 160) completeCalibrationMeasurement();
    }
    }
  }
  if (measuringCalibration && millis()-calibrationStarted >= 3000) {
    measuringCalibration = false;
    Serial.printf("CAL timeout valid_samples=%u\n", calibrationSamples);
    screen(state == CAL_EMPTY ? "CALIBRATION 1/2" : "CALIBRATION 2/2",
      "Sensor read timeout", "Check I2C connection", "Press/release to retry");
  }
  if (state == RECORDING && (writeError || now-lastSampleUs > 100000)) {
    writeError = true; stop();
  }
  if (state == SAVING && writerDone.load(std::memory_order_acquire)) {
    // Append quality counters only after writer has closed the data file.
    File summary = SD_MMC.open(filename, FILE_APPEND);
    if (!summary) writeError = true;
    else {
      summary.printf("# acquired=%lu,dropped=%lu,gaps_over_5ms=%lu,adc_clipped=%lu,error=%u\n",
        (unsigned long)samples,(unsigned long)dropped,(unsigned long)gaps,(unsigned long)clipped,unsigned(writeError.load()));
      summary.flush(); if (summary.getWriteError()) writeError = true; summary.close();
    }
    state = POST; color(writeError || dropped || clipped || gaps ? 40 : 0,40,0);
    char report[32]; snprintf(report,sizeof(report),"%lu samples; lost %lu",(unsigned long)samples,(unsigned long)dropped);
    screen(writeError ? "RECORDING ERROR" : (dropped || clipped || gaps ? "SAVED / CHECK DATA" : "SAVED"),filename,report,"Press for READY");
    Serial.printf("%s samples=%lu dropped=%lu gaps=%lu clipped=%lu error=%u\n",filename,
      (unsigned long)samples,(unsigned long)dropped,(unsigned long)gaps,(unsigned long)clipped,unsigned(writeError.load()));
  }
  // Debounce both edges; actions on release prevent a long hold also starting a run.
  static bool previous = HIGH, stable = HIGH, longHandled = false;
  static uint32_t changed = 0, pressed = 0;
  bool input = digitalRead(BUTTON); uint32_t ms = millis();
  if (input != previous) { previous = input; changed = ms; if (state != RECORDING) Serial.printf("BUTTON level=%u\n", unsigned(input)); }
  if (ms-changed >= 30 && input != stable) {
    stable = input;
    if (!stable) { pressed = ms; longHandled = false; }
    else if (!longHandled) { if (state != RECORDING) Serial.printf("BUTTON short state=%u\n", unsigned(state)); shortPress(); }
  }
  if (!stable && !longHandled && ms-pressed >= 2000 && state == READY) {
    longHandled = true; state = CAL_EMPTY; color(30,0,30);
    screen("CALIBRATION 1/2", "Remove all load", "Wait until stable", "Press to measure");
  }
  static uint32_t displayAt = 0;
  if (state == READY && oledPosition >= 1024 && ms-displayAt >= 250) {
    displayAt = ms;
    if (countsPerGram == 0) screen("READY / UNCALIBRATED", "Hold button 2 sec", "to calibrate with 629g", "", true);
    else {
      char weight[32]; snprintf(weight,sizeof(weight),"%.1f g",haveSample ? (double(latest)-zero)/countsPerGram : 0.0);
      screen("READY",weight,"Press: tare + record","Hold 2s: calibrate", true);
    }
  }
  updateReadyDisplay();
  // No OLED transactions during acquisition: a full frame would steal ADC periods.
  delay(1);
}
