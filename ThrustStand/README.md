# CWA thrust stand

Standalone Arduino firmware for CWA v2+ ESP32-S3, NAU7802, SSD1306 128×64 at 0x3C, and native four-bit SD_MMC. It does not control ignition. The existing RocketLogger sketch is independent.

## Wiring

| Signal | GPIO |
|---|---:|
| Button (active low, onboard BOOT) | 0 |
| NeoPixel | 39 |
| I2C SDA / SCL | 17 / 18 |
| SD CLK / CMD | 36 / 37 |
| SD D0 / D1 / D2 / D3 | 38 / 34 / 33 / 35 |

NAU7802 address is 0x2A; connect load cell to channel 1. Use a proper 3.3 V native SD socket with required pull-ups on CMD and DAT0–DAT3; SPI-only modules are unsuitable. Insert a FAT32 SD card before power-on. Firmware never formats cards. No NAU7802 DRDY wire is required.

## Operation

- Blue: live grams, refreshed approximately four times a second. READY display data are sent in 16-byte chunks so a full OLED refresh cannot hide a short button press. First use requires calibration.
- Hold button for two seconds while blue: purple calibration wizard. Remove all load, allow settling, then press/release to measure empty. Place the 629 g reference, allow settling, then press/release. Scale and empty offset are saved together in NVS and read back to verify the save. On startup the saved values are loaded automatically; the load-cell calibration wizard starts only after a two-second hold while blue. Each measurement collects 160 valid readings without blocking the button handler, with a three-second timeout and an on-screen retry prompt. Remove the reference to check zero. Recalibrate after changing the fixture or sensor wiring. Calibration load must act in the same direction as thrust.
- Short press/release while blue: average the mounted motor's static load for about 0.5 seconds, open a fresh file, then turn red and record. Wait for red before firing. Time zero is acquisition start, not ignition. Motor weight is removed by this run's tare, without changing stored calibration.
- Short press/release while red: stop. Otherwise stop automatically after 180 seconds from acquisition start. Long holds while recording also stop on release.
- OLED shows SAVING until queued data are written and the file closed. Green means saved; yellow means inspect quality counters or recording error. Press/release returns to blue. Purple ERROR requires a reset after fixing the indicated problem.

Do not remove the card or power while red or SAVING. Green follows a flush and close; sudden power loss can lose buffered data or damage the current filesystem. Finished files are THR00001.csv, THR00002.csv, etc.; existing files are never intentionally reused. Remove the card after saving to read it on a computer.

## Capture and quality

NAU7802 runs at nominal 320 samples/s (3.125 ms per conversion), gain 128. The foreground reads each available conversion without averaging. CSV columns are elapsed_ms (three decimal places), signed raw_adc, grams, thrust_N. Newtons = grams × 0.00980665. Acquisition timestamps use the ESP32 microsecond clock at read completion; polling and I2C add latency. Sensor filtering limits signal bandwidth; microsecond timestamp precision does not imply microsecond sensor response.

The writer runs on core 0. A 2048-record RAM queue provides about 6.4 seconds of SD stall tolerance. Four-KiB batches avoid one SD write per sample. The OLED stays static during recording to avoid blocking the shared I2C bus. A sensor timeout or detected write failure ends capture. The final comment contains acquired samples, queue drops, intervals exceeding 5 ms, ADC rail hits, and an error flag. Gap counts flag possible missed conversions; the ADC provides no sequence number, so exact conversion loss cannot be proven by polling. Treat files with errors, drops, gaps, or clipping as suspect. CSV readers should ignore lines beginning with #.

The load cell's reported 10 kg capacity is unverified. The firmware does not assume that motor class guarantees a safe peak load, and ADC clipping detection does not establish mechanical load-cell capacity. Verify the actual sensor rating and fixture loads. A 629 g reference establishes sensitivity near that load; it does not verify full-range linearity.

## Build

Open ThrustStand.ino in Arduino IDE, retaining Stand.cpp beside it. Use ESP32S3 Dev Module, 8 MB flash, default 8 MB partitions, USB CDC enabled, PSRAM disabled. Install the versions listed in sketch.yaml through Library Manager, or use Arduino CLI's pinned profile:

```sh
arduino-cli compile --profile cwa_v2 --build-path build/thrust ThrustStand
arduino-cli board list
arduino-cli upload --profile cwa_v2 --port YOUR_PORT --input-dir build/thrust ThrustStand
```

On an ARM Mac with the bundled Intel ctags issue, add `--build-property runtime.tools.ctags.path=/opt/homebrew/bin` to compile. Upload replaces whichever firmware is currently installed. Serial status is 115200 baud; logging does not require a computer.

Before a motor test, calibrate, confirm unloaded zero and 629 g reading, record a short applied-weight run, and inspect its CSV and quality footer. Also exercise manual stop and the 180-second limit with inert loads. Hardware rate and SD stall performance must be measured on the actual assembly.

Bench diagnostics at 115200 baud: `?` reports state, button level, raw ADC, grams, calibration factor, and card size; `r` reads back the last file from this boot when idle. Calibration stage transitions and measured raw differences are also printed. I2C runs at 100 kHz; SD initialization uses 10 MHz in four-bit mode with three startup attempts.

## Validation status

The pinned ESP32-S3 build compiled and uploaded successfully. Bench queries confirmed SD mounting, live ADC readings, and saved calibration loading after a firmware restart. Calibration completion was confirmed by the operator. The latest change splits READY display refreshes into small transfers to improve short-press detection; the complete start/stop capture flow, CSV quality, and three-minute cutoff still require bench verification. These results apply to ThrustStand; RocketLogger's existing validation records concern a different sketch.
