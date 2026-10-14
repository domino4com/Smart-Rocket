/* CWA v2+ rocket flight logger. Arduino ESP32S3 Dev Module, 8MB flash,
 * USB CDC enabled. See Config.h for pins and tuning.
 * Sensor task owns I2C; storage task owns every SD operation. A RAM queue
 * keeps SD latency out of acquisition. CSV reports gaps and queue losses.
 * Serial commands: ? status, r start bench recording, s stop, l list files,
 * d dump last CSV, t create a status TXT. Play stops recording and saves post-flight status.
 * No WiFi/Bluetooth. PMU battery telemetry is never read while recording.
 */
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_timer.h>

#include <atomic>

#include "Config.h"

XPowersAXP2101 pmu(Wire, SDA_PIN, SCL_PIN);
Adafruit_NeoPixel pixel(1, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
enum State { READY, FLIGHT, CLOSING, POST, FAULT, POWER_OFF };
State state = READY;
std::atomic<bool> storageFault{false}, opened{false}, closed{false};
std::atomic<uint32_t> written{0}, maxWriteUs{0};
std::atomic<bool> dumping{false};
uint32_t acquired = 0, dropped = 0, i2cErrors = 0, maxGapUs = 0, baroCount = 0;
uint32_t lastAccelUs = 0, baroMs = 0, tempMs = 0, flightStart = 0, flightSeq = 0;
float ax = NAN, ay = NAN, az = NAN, tempC = NAN, pressurePa = NAN, scaledT = 0;
int c0, c1, c00, c10, c01, c11, c20, c21, c30;
bool haveT = false, haveP = false, pmuOK = false, shutAfterClose = false;
bool stopRequested = false, reportAfterClose = false;
bool sensorsOK = false, sdOK = false;
bool pmuOffObserved = false;
struct Battery {
  int soc = -1;
  uint16_t mv = 0, vbusMv = 0, systemMv = 0;
  bool present = false, usb = false, charging = false;
  uint32_t at = 0;
} battery;
struct Item {
  uint8_t type;  // 0 sample, 1 open, 2 close, 3 status, 4 list, 5 dump
  uint32_t ms, us, seq, baroAt, tempAt, loss;
  float x, y, z, t, p;
  uint8_t mode;
};
QueueHandle_t queue;
Battery statusBatteries[4];
std::atomic<bool> statusUsed[4]{};
char lastFile[40] = "";
char lastStatusFile[40] = "";
TaskHandle_t sensorTask;
esp_timer_handle_t tickTimer;

// Checked burst reads: incomplete transfers are errors, never zero-valued data.
bool readRegs(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    i2cErrors++;
    return false;
  }
  if (Wire.requestFrom(addr, n) != n) {
    while (Wire.available()) Wire.read();
    i2cErrors++;
    return false;
  }
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}
bool putReg(uint8_t addr, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(value);
  if (Wire.endTransmission() != 0) {
    i2cErrors++;
    return false;
  }
  return true;
}
int32_t signExtend(uint32_t v, int bits) {
  return (v & (1UL << (bits - 1))) ? int32_t(v) - int32_t(1UL << bits) : int32_t(v);
}
int32_t raw24(uint8_t* b) {
  return signExtend((uint32_t(b[0]) << 16) | (uint32_t(b[1]) << 8) | b[2], 24);
}
bool initSensors() {
  delay(100);  // Allow sensor POR after enabling/restarting the PMU rail.
  uint8_t id;
  if (!readRegs(0x0F, 0x0F, &id, 1) || id != 0x35) return false;
  // KXTJ3: standby, configured ODR, 14-bit high resolution, ±16g, data-ready enabled.
  uint8_t rate = ACCEL_HZ == 1600 ? 7 : ACCEL_HZ == 800 ? 6 : 5;
  if (!putReg(0x0F, 0x1B, 0) || !putReg(0x0F, 0x21, rate) || !putReg(0x0F, 0x1E, 0) ||
      !putReg(0x0F, 0x1B, 0xFC))
    return false;
  delay(50);
  if (!readRegs(0x77, 0x0D, &id, 1)) return false;
  Serial.printf("SPL06 ID=0x%02x\n", id);
  if (id != 0x10) return false;
  if (!putReg(0x77, 0x0C, 0x09)) return false;
  delay(100);
  if (!readRegs(0x77, 0x08, &id, 1) || (id & 0xC0) != 0xC0) return false;
  uint8_t b[18], source;
  if (!readRegs(0x77, 0x10, b, 18) || !readRegs(0x77, 0x28, &source, 1)) return false;
  // Cache factory compensation once, rather than rereading it on each sample.
  c0 = signExtend((b[0] << 4) | (b[1] >> 4), 12);
  c1 = signExtend(((b[1] & 15) << 8) | b[2], 12);
  c00 = signExtend((uint32_t(b[3]) << 12) | (b[4] << 4) | (b[5] >> 4), 20);
  c10 = signExtend((uint32_t(b[5] & 15) << 16) | (b[6] << 8) | b[7], 20);
  c01 = signExtend((b[8] << 8) | b[9], 16);
  c11 = signExtend((b[10] << 8) | b[11], 16);
  c20 = signExtend((b[12] << 8) | b[13], 16);
  c21 = signExtend((b[14] << 8) | b[15], 16);
  c30 = signExtend((b[16] << 8) | b[17], 16);
  // IWB requires the external MEMS temperature sensor (TMP_EXT=1).
  // 128 pressure/s at 1x, 32 temperature/s at 1x. Combined conversion
  // budget: 128*3.6ms + 32*3.6ms < 1s. No FIFO or result shifts needed.
  return putReg(0x77, 0x06, 0x70) && putReg(0x77, 0x07, 0xD0) && putReg(0x77, 0x09, 0) &&
         putReg(0x77, 0x08, 7);
}

void refreshBattery() {
  if (!pmuOK || state == FLIGHT || state == CLOSING) return;
  battery.present = pmu.isBatteryConnect();
  battery.soc = battery.present ? pmu.getBatteryPercent() : -1;
  battery.mv = pmu.getBattVoltage();
  if (battery.mv < 2500 || battery.mv > 4350)
    battery.soc = -1;  // invalid voltage makes SoC untrustworthy
  battery.vbusMv = pmu.getVbusVoltage();
  battery.systemMv = pmu.getSystemVoltage();
  battery.usb = pmu.isVbusIn();
  battery.charging = pmu.isCharging();
  battery.at = millis();
  pmu.setChargingLedMode(battery.charging ? XPOWERS_CHG_LED_BLINK_1HZ : XPOWERS_CHG_LED_ON);
}
bool initPower() {
  if (!pmu.init()) return false;
  pmu.disableCellbatteryCharge();
  pmu.disableTSPinMeasure();  // schematic uses a fixed 10k to ground, no thermistor
  pmu.enableGeneralAdcChannel();
  pmu.enableBattDetection();
  pmu.enableBattVoltageMeasure();
  pmu.enableVbusVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  if (!pmu.setDC1Voltage(3300) || !pmu.enableDC1()) return false;
  // No light-load shutdown or sleep: keep DCDC1 enabled continuously.
  pmu.disableWatchdog();
  pmu.enableLongPressShutdown();
  pmu.setLongPressPowerOFF();
  pmu.enablePwronShutPMIC();
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);
  pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_1S);
  pmu.disableInterrupt(XPOWERS_ALL_INT);
  pmu.enableInterrupt(XPOWERS_PWR_BTN_LONGPRESSED_INT);
  pmu.clearIrqStatus();
  if (ENABLE_CHARGING) {
    pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_25MA);
    pmu.setChargerConstantCurr(CHARGE_CURRENT_MA == 100 ? XPOWERS_AXP2101_CHG_CUR_100MA
                                                        : XPOWERS_AXP2101_CHG_CUR_200MA);
    pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
    pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_25MA);
    pmu.enableChargerTerminationLimit();
    pmu.enableCellbatteryCharge();
  }
  uint8_t offConfig = 0, keyTiming = 0, keyMask = 0;
  readRegs(0x34, 0x22, &offConfig, 1);
  readRegs(0x34, 0x27, &keyTiming, 1);
  readRegs(0x34, 0x41, &keyMask, 1);
  Serial.printf("PMU DCDC1=%umV enabled=%d PKEY off_config=%02x timing=%02x irq_mask=%02x\n",
                pmu.getDC1Voltage(), pmu.isEnableDC1(), offConfig, keyTiming, keyMask);
  return pmu.getDC1Voltage() == 3300 && pmu.isEnableDC1();
}

// This task alone touches SD, including status reports and diagnostic readback.
void storageTask(void*) {
  File file;
  char buffer[8192];
  size_t used = 0;
  uint32_t lastFlush = millis();
  uint32_t nextFlight = 1, nextStatus = 1;
  // Recover the latest saved names after USB/power resets. This scans only
  // our numbered files; existing card contents are never deleted or rewritten.
  File root = SD_MMC.open("/");
  for (File saved = root.openNextFile(); saved; saved = root.openNextFile()) {
    if (!saved.isDirectory()) {
      String name = saved.name();
      if (name.startsWith("/")) name.remove(0, 1);
      if (name.startsWith("flight_") && name.endsWith(".csv")) {
        uint32_t number = name.substring(7, name.length() - 4).toInt();
        if (number >= nextFlight) {
          nextFlight = number + 1;
          snprintf(lastFile, sizeof lastFile, "/%s", name.c_str());
        }
      } else if (name.startsWith("status_") && name.endsWith(".txt")) {
        uint32_t number = name.substring(7, name.length() - 4).toInt();
        if (number >= nextStatus) {
          nextStatus = number + 1;
          snprintf(lastStatusFile, sizeof lastStatusFile, "/%s", name.c_str());
        }
      }
    }
    saved.close();
  }
  root.close();

  auto flushBuffer = [&]() {
    if (!used) return true;
    size_t expected = used;
    uint32_t t = micros();
    size_t n = file.write((uint8_t*)buffer, used);
    uint32_t elapsed = micros() - t;
    if (elapsed > maxWriteUs) maxWriteUs = elapsed;
    used = 0;
    if (n != expected) return false;
    return true;
  };
  Item item;
  Battery reportBat;
  for (;;) {
    if (xQueueReceive(queue, &item, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (item.type == 1) {
        do {
          snprintf(lastFile, sizeof lastFile, "/flight_%06lu.csv", (unsigned long)nextFlight++);
        } while (SD_MMC.exists(lastFile));
        file = SD_MMC.open(lastFile, FILE_WRITE);
        written = 0;
        used = 0;
        lastFlush = millis();
        if (!file)
          storageFault = true;
        else {
          file.println(
              "millis,elapsed_ms,micros,seq,x_g,y_g,z_g,temp_C,pressure_Pa,pressure_age_ms,temp_"
              "age_ms,dropped_total");
          file.flush();
          opened = true;
          Serial.printf("OPEN %s\n", lastFile);
        }
      } else if (item.type == 0 && file) {
        char row[200];
        int n = snprintf(row, sizeof row, "%lu,%lu,%lu,%lu,%.5f,%.5f,%.5f,%.3f,%.2f,%lu,%lu,%lu\n",
                         (unsigned long)item.ms, (unsigned long)(item.ms - flightStart),
                         (unsigned long)item.us, (unsigned long)item.seq, item.x, item.y, item.z,
                         item.t, item.p, (unsigned long)(item.ms - item.baroAt),
                         (unsigned long)(item.ms - item.tempAt), (unsigned long)item.loss);
        if (n <= 0 || size_t(n) >= sizeof row)
          storageFault = true;
        else {
          if (used + n > sizeof buffer && !flushBuffer()) storageFault = true;
          memcpy(buffer + used, row, n);
          used += n;
          written++;
        }
      } else if (item.type == 2) {
        if (file) {
          if (!flushBuffer()) storageFault = true;
          file.flush();
          file.close();
        }
        closed = true;
        Serial.printf("CLOSED rows=%lu max_write_us=%lu\n", (unsigned long)written.load(),
                      (unsigned long)maxWriteUs.load());
      } else if (item.type == 3) {
        reportBat = statusBatteries[item.us];
        statusUsed[item.us] = false;
        char path[40];
        do {
          snprintf(path, sizeof path, "/status_%06lu.txt", (unsigned long)nextStatus++);
        } while (SD_MMC.exists(path));
        strlcpy(lastStatusFile, path, sizeof lastStatusFile);
        File f = SD_MMC.open(path, FILE_WRITE);
        if (!f) {
          storageFault = true;
          continue;
        }
        f.printf(
            "RocketLogger\nuptime_ms=%lu\nstate=%u\nx_g=%.5f\ny_g=%.5f\nz_g=%.5f\ntemp_C=%."
            "3f\npressure_Pa=%.2f\nbattery_present=%d\nsoc_percent=%d\nbattery_mV=%u\nvbus_mV=%"
            "u\nsystem_mV=%u\nusb_present=%d\ncharging=%d\ncharging_enabled=%d\nbattery_snapshot_"
            "ms=%lu\nbattery_snapshot_age_ms=%lu\naccel_hz=%u\nrange_g=16\nacquired=%lu\ndropped=%"
            "lu\ni2c_errors=%lu\nmax_gap_us=%lu\nlast_csv=%s\n",
            (unsigned long)item.ms, item.mode, item.x, item.y, item.z, item.t, item.p,
            reportBat.present, reportBat.soc, reportBat.mv, reportBat.vbusMv, reportBat.systemMv,
            reportBat.usb, reportBat.charging, ENABLE_CHARGING, (unsigned long)reportBat.at,
            (unsigned long)(item.ms - reportBat.at), ACCEL_HZ, (unsigned long)item.seq,
            (unsigned long)item.loss, (unsigned long)item.baroAt, (unsigned long)item.tempAt,
            lastFile);
        f.flush();
        f.close();
        Serial.printf("STATUS %s\n", path);
      } else if (item.type == 4) {
        File root = SD_MMC.open("/");
        for (File f = root.openNextFile(); f; f = root.openNextFile()) {
          Serial.printf("FILE %s %lu\n", f.name(), (unsigned long)f.size());
          f.close();
        }
        root.close();
      } else if ((item.type == 5 && lastFile[0]) || (item.type == 6 && lastStatusFile[0])) {
        dumping = true;
        // File transfer is a bench operation. Permit USB backpressure here,
        // with arming/other console commands suspended until it finishes.
        // Normal recording keeps USB writes nonblocking.
        Serial.setTxTimeoutMs(1000);
        File f = SD_MMC.open(item.type == 5 ? lastFile : lastStatusFile);
        Serial.println(item.type == 5 ? "CSV_BEGIN" : "TXT_BEGIN");
        uint8_t chunk[512];
        while (f && f.available()) {
          size_t n = f.read(chunk, sizeof chunk);
          size_t sent = 0;
          uint32_t start = millis();
          while (sent < n && millis() - start < 5000) {
            sent += Serial.write(chunk + sent, n - sent);
            vTaskDelay(1);
          }
          if (sent != n) break;  // disconnected host; caller must validate the transfer
          vTaskDelay(1);
        }
        f.close();
        Serial.println(item.type == 5 ? "CSV_END" : "TXT_END");
        Serial.setTxTimeoutMs(0);
        dumping = false;
      }
    }
    if (file && millis() - lastFlush >= 500) {
      if (!flushBuffer()) storageFault = true;
      uint32_t t = micros();
      file.flush();
      uint32_t elapsed = micros() - t;
      if (elapsed > maxWriteUs) maxWriteUs = elapsed;
      lastFlush = millis();
    }
  }
}

bool sendControl(uint8_t type) {
  Item i{};
  i.type = type;
  i.ms = millis();
  i.mode = state;
  i.x = ax;
  i.y = ay;
  i.z = az;
  i.t = tempC;
  i.p = pressurePa;
  i.seq = acquired;
  i.loss = dropped;
  i.baroAt = i2cErrors;
  i.tempAt = maxGapUs;
  int slot = -1;
  if (type == 3) {
    for (int j = 0; j < 4; j++) {
      bool empty = false;
      if (statusUsed[j].compare_exchange_strong(empty, true)) {
        slot = j;
        break;
      }
    }
    if (slot < 0) return false;
    statusBatteries[slot] = battery;
    i.us = slot;
  }
  bool ok = xQueueSend(queue, &i, 0) == pdTRUE;
  if (!ok && slot >= 0) statusUsed[slot] = false;
  return ok;
}
void startFlight(uint32_t startMs = millis()) {
  if (state != READY && state != POST) return;
  if (!haveT || !haveP || storageFault) return;
  refreshBattery();
  // Use the triggering sample time so pre-flight battery reads cannot make
  // the first elapsed timestamp wrap below zero.
  flightStart = startMs;
  flightSeq = 0;
  dropped = 0;
  maxGapUs = 0;
  maxWriteUs = 0;
  opened = false;
  closed = false;
  if (sendControl(1)) {
    state = FLIGHT;
    Serial.println("STATE FLIGHT");
  }
}
void stopFlight() {
  if (state != FLIGHT) return;
  // Latch a stop request so an SD backlog cannot swallow a button press.
  stopRequested = true;
  if (sendControl(2)) {
    stopRequested = false;
    state = CLOSING;
    Serial.println("STATE CLOSING");
  }
}
void statusReport() {
  if (state != FLIGHT && state != CLOSING) refreshBattery();
  if (!sendControl(3)) Serial.println("Status queue full");
}
// PMU key handling is independent of sensor/SD health. A sensor fault must
// not disable the power button. Hardware shutdown at 10s remains a backup.
void performShutdown() {
  Serial.println("POWER: requesting PMU shutdown (USB/LDO may still power ESP32)");
  state = POWER_OFF;
  pmuOffObserved = false;
  pixel.clear();
  pixel.show();
  pmu.setChargingLedMode(pmu.isCharging() ? XPOWERS_CHG_LED_BLINK_1HZ : XPOWERS_CHG_LED_OFF);
  pmu.shutdown();
  delay(100);
  pmuOffObserved = !pmu.isPowerOn();
  Serial.printf("POWER: pmu_on=%d (0 confirms PMU off)\n", !pmuOffObserved);
}
void pollPowerKey(uint32_t now) {
  static uint32_t lastKeyPoll = 0;
  if (!pmuOK || state == POWER_OFF || shutAfterClose || now - lastKeyPoll < 100) return;
  lastKeyPoll = now;
  pmu.getIrqStatus();
  bool longPress = pmu.isPekeyLongPressIrq();
  pmu.clearIrqStatus();
  if (!longPress) return;
  Serial.println("POWER: long-press IRQ detected");
  shutAfterClose = true;
  if (state == FLIGHT)
    stopFlight();
  else if (state != CLOSING)
    performShutdown();
}

void pixelState() {
  uint32_t color;
  if (state == FAULT)
    color = pixel.Color(40, 15, 0);
  else if (state == FLIGHT || state == CLOSING)
    color = pixel.Color(40, 0, 0);
  else {
    int soc = constrain(battery.soc < 0 ? 0 : battery.soc, 0, 100);
    uint32_t period = 250 + soc * 27;  // 4Hz empty to ~0.34Hz full, 50% duty
    bool on = (millis() % period) < period / 2;
    color = !on                ? 0
            : battery.charging ? pixel.Color(24, 0, 24)
            : state == POST    ? pixel.Color(0, 32, 0)
                               : pixel.Color(0, 0, 32);
  }
  static uint32_t previous = 0xFFFFFFFF;
  if (previous != color) {
    pixel.setPixelColor(0, color);
    pixel.show();
    previous = color;
  }
}
void timerTick(void*) { xTaskNotifyGive(sensorTask); }
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // An absent/slow USB host must never stall flight sampling.
  delay(1000);
  Serial.println("RocketLogger boot");
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pixel.begin();
  Wire.begin(SDA_PIN, SCL_PIN, 400000);
  Wire.setTimeOut(10);
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf("I2C 0x%02x\n", a);
  }
  pmuOK = initPower();
  Wire.setClock(400000);
  sensorsOK = initSensors();
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  sdOK = SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_HIGHSPEED);  // 4-bit, no auto-format
  Serial.printf("INIT pmu=%d sensors=%d sd_4bit=%d card_MB=%llu\n", pmuOK, sensorsOK, sdOK,
                sdOK ? SD_MMC.cardSize() / 1048576 : 0);
  if (!pmuOK || !sensorsOK || !sdOK) {
    state = FAULT;
    pixelState();
    return;
  }
  refreshBattery();
  queue = xQueueCreate(4096, sizeof(Item));
  if (!queue ||
      xTaskCreatePinnedToCore(storageTask, "SD writer", 16384, nullptr, 1, nullptr, 0) != pdPASS) {
    state = FAULT;
    return;
  }
  sensorTask = xTaskGetCurrentTaskHandle();
  vTaskPrioritySet(sensorTask, 3);
  esp_timer_create_args_t args{};
  args.callback = timerTick;
  args.name = "sample poll";
  if (esp_timer_create(&args, &tickTimer) != ESP_OK ||
      esp_timer_start_periodic(tickTimer, 400) != ESP_OK) {
    state = FAULT;
    return;
  }
  Serial.printf("READY commands: ? r s l d t v heap=%u queue_bytes=%u\n", ESP.getFreeHeap(),
                4096 * sizeof(Item));
}
void loop() {
  if (state == POWER_OFF) {
    // USB bypasses the PMU. Halt acquisition and LEDs while that supply remains.
    bool on = pmu.isPowerOn();
    if (!on) pmuOffObserved = true;
    if (pmuOffObserved && on) ESP.restart();  // PMU key turned its rails back on
    if (Serial.available() && Serial.read() == '?')
      Serial.printf("STATUS state=5 pmu_on=%d USB_LDO_can_still_power_ESP32=1\n", on);
    delay(100);
    return;
  }
  pollPowerKey(millis());
  if (state == POWER_OFF) return;
  if (state == FAULT) {
    // Keep startup faults inspectable after reconnecting the USB monitor.
    if (Serial.available() && Serial.read() == '?') {
      uint8_t accelId = 0xff, baroId = 0xff;
      bool accelAck = readRegs(0x0f, 0x0f, &accelId, 1);
      bool baroAck = readRegs(0x77, 0x0d, &baroId, 1);
      Serial.printf(
          "STATUS state=4 pmu=%d sensors=%d sd_4bit=%d storage_fault=%d i2c_errors=%lu "
          "accel_ack=%d accel_id=%02x baro_ack=%d baro_id=%02x\n",
          pmuOK, sensorsOK, sdOK, storageFault.load(), (unsigned long)i2cErrors, accelAck, accelId,
          baroAck, baroId);
    }
    pixelState();
    delay(100);
    return;
  }
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  uint32_t now = millis();
  uint8_t ready;
  static uint32_t lastBaroPoll = 0, lastTelemetry = 0, lastPrint = 0, lastLed = 0;
  if (now - lastBaroPoll >= 2) {
    lastBaroPoll = now;
    if (readRegs(0x77, 0x08, &ready, 1)) {
      uint8_t b[3];
      if ((ready & 0x20) && readRegs(0x77, 3, b, 3)) {
        scaledT = raw24(b) / 524288.0f;
        tempC = c0 * 0.5f + c1 * scaledT;
        haveT = true;
        tempMs = now;
      }
      if ((ready & 0x10) && haveT && readRegs(0x77, 0, b, 3)) {
        float p = raw24(b) / 524288.0f;
        pressurePa =
            c00 + p * (c10 + p * (c20 + p * c30)) + scaledT * c01 + scaledT * p * (c11 + p * c21);
        haveP = true;
        baroMs = now;
        baroCount++;
      }
    }
  }
  static uint32_t invertedAt = 0, uprightAt = 0;
  static bool armEligible = true;
  static float orientation[3] = {NAN, NAN, NAN};
  if (readRegs(0x0F, 0x16, &ready, 1) && (ready & 0x10)) {
    uint8_t b[6];
    if (readRegs(0x0F, 0x06, b, 6)) {
      uint32_t us = micros();
      uint32_t deltaUs = lastAccelUs ? us - lastAccelUs : 1250;
      if (lastAccelUs && us - lastAccelUs > maxGapUs) maxGapUs = us - lastAccelUs;
      lastAccelUs = us;
      ax = int16_t((b[1] << 8) | b[0]) / 2048.0f;
      ay = int16_t((b[3] << 8) | b[2]) / 2048.0f;
      az = int16_t((b[5] << 8) | b[4]) / 2048.0f;
      acquired++;
      // Smooth the gravity vector for inversion detection. Requiring hundreds
      // of consecutive unfiltered samples would make arming noise-sensitive.
      float vOrientation[3] = {ax, ay, az};
      float alpha = fminf(1.0f, deltaUs / 50000.0f);
      for (int j = 0; j < 3; j++) {
        if (!isfinite(orientation[j]))
          orientation[j] = vOrientation[j];
        else
          orientation[j] += alpha * (vOrientation[j] - orientation[j]);
      }
      float armNorm = sqrtf(orientation[0] * orientation[0] + orientation[1] * orientation[1] +
                            orientation[2] * orientation[2]);
      if ((state == READY || state == POST) && !dumping) {
        if (orientation[0] > 0.75f && armNorm > ARM_GRAVITY_MIN_G && armNorm < ARM_GRAVITY_MAX_G) {
          if (!uprightAt) uprightAt = now;
          if (now - uprightAt >= 1000) armEligible = true;
        } else
          uprightAt = 0;
        if (armEligible && orientation[0] < ARM_AXIS_G && armNorm > ARM_GRAVITY_MIN_G &&
            armNorm < ARM_GRAVITY_MAX_G) {
          if (!invertedAt) invertedAt = now;
          if (now - invertedAt >= 500) {
            startFlight(now);
            // Consume the gesture only after a recording actually starts.
            if (state == FLIGHT) {
              armEligible = false;
              invertedAt = 0;
            } else {
              invertedAt = now;  // retry after another hold if startup was not ready
            }
          }
        } else
          invertedAt = 0;
      }
      if (state == FLIGHT) {
        // Remain recording even when stationary on the launch pad.
        // Only Play, a serial stop command, PMU shutdown, or a fault closes it.
        Item i{};
        i.ms = now;
        i.us = us;
        i.seq = flightSeq++;
        i.x = ax;
        i.y = ay;
        i.z = az;
        i.t = tempC;
        i.p = pressurePa;
        i.baroAt = baroMs;
        i.tempAt = tempMs;
        i.loss = dropped;
        if (xQueueSend(queue, &i, 0) != pdTRUE) dropped++;
      }
    }
  }
  // A failed accelerometer must not leave an apparently healthy recording.
  if (lastAccelUs && micros() - lastAccelUs > 1000000) {
    stopFlight();
    storageFault = true;
  }
  if (stopRequested && state == FLIGHT) stopFlight();
  if (storageFault) {
    stopFlight();
    if (state != FLIGHT && state != CLOSING) state = FAULT;
  }
  if (state == CLOSING && closed) {
    state = storageFault ? FAULT : POST;
    armEligible = false;
    Serial.println("STATE POST");
    if (reportAfterClose) {
      reportAfterClose = false;
      statusReport();  // recording is closed; fresh battery telemetry is allowed
    }
    if (shutAfterClose) {
      performShutdown();
      return;
    }
  }
  static bool buttonRaw = false, buttonStable = false;
  static uint32_t buttonAt = 0;
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed != buttonRaw) {
    buttonRaw = pressed;
    buttonAt = now;
  }
  if (now - buttonAt >= 30 && buttonStable != buttonRaw) {
    buttonStable = buttonRaw;
    if (buttonStable) {
      if (state == FLIGHT || state == CLOSING) {
        reportAfterClose = true;
        stopFlight();
      } else {
        statusReport();
      }
    }
  }
  if (now - lastTelemetry >= 1000) {
    lastTelemetry = now;
    refreshBattery();
  }
  if (!dumping && Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'r') startFlight();
    if (cmd == 's') stopFlight();
    if (cmd == 't') statusReport();
    if ((cmd == 'l' || cmd == 'd' || cmd == 'b') && state != FLIGHT && state != CLOSING)
      sendControl(cmd == 'l' ? 4 : cmd == 'd' ? 5 : 6);
    if (cmd == 'v' && state != FLIGHT && state != CLOSING) {
      for (int r = 0; r < 0x40; r++) {
        uint8_t b;
        if (readRegs(0x34, r, &b, 1)) Serial.printf("PMU_REG %02x=%02x\n", r, b);
      }
      Serial.printf("PMU present=%d system=%u vbus=%u battery=%u soc=%d\n", battery.present,
                    battery.systemMv, battery.vbusMv, battery.mv, battery.soc);
    }
    if (cmd == '?') {
      Serial.printf(
          "STATUS state=%d x=%.5f y=%.5f z=%.5f temp=%.3f pressure=%.2f soc=%d batt_mV=%u usb=%d "
          "charging=%d age_ms=%lu samples=%lu baro=%lu dropped=%lu errors=%lu max_gap_us=%lu "
          "rows=%lu\n",
          state, ax, ay, az, tempC, pressurePa, battery.soc, battery.mv, battery.usb,
          battery.charging, (unsigned long)(millis() - battery.at), (unsigned long)acquired,
          (unsigned long)baroCount, (unsigned long)dropped, (unsigned long)i2cErrors,
          (unsigned long)maxGapUs, (unsigned long)written.load());
      Serial.printf("ARM eligible=%d filtered_x=%.4f norm=%.4f inverted_hold_ms=%lu\n", armEligible,
                    orientation[0],
                    sqrtf(orientation[0] * orientation[0] + orientation[1] * orientation[1] +
                          orientation[2] * orientation[2]),
                    (unsigned long)(invertedAt ? millis() - invertedAt : 0));
    }
  }
  if (!dumping && now - lastPrint >= 5000) {
    lastPrint = now;
    Serial.printf(
        "LIVE state=%d xyz=%.4f,%.4f,%.4f temp=%.2f Pa=%.2f samples=%lu baro=%lu drops=%lu "
        "errors=%lu gap_us=%lu\n",
        state, ax, ay, az, tempC, pressurePa, (unsigned long)acquired, (unsigned long)baroCount,
        (unsigned long)dropped, (unsigned long)i2cErrors, (unsigned long)maxGapUs);
  }
  if (now - lastLed >= 20) {
    lastLed = now;
    pixelState();
  }
}
