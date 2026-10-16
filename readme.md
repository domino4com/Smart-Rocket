# CWA v2+ rocket logger

For the separate static motor thrust stand (NAU7802 load cell, SSD1306 OLED, saved 629 g calibration, and four-bit SD recording), see [ThrustStand](ThrustStand/README.md).

Open `RocketLogger/RocketLogger.ino` in Arduino IDE. The commented implementation is in the adjacent `Logger.cpp`; settings are in `Config.h`.

## Files to share

Share the repository, or share the complete `RocketLogger/` folder together with this README. Do not share only the `.ino`: Arduino also needs `Logger.cpp`, `Config.h`, `PowerPolicy.h`, and `sketch.yaml`. No local library folders or virtual environment are required.

| File | Purpose |
|---|---|
| `RocketLogger/RocketLogger.ino` | Arduino sketch entry file |
| `RocketLogger/Logger.cpp` | Commented firmware, sensor drivers, state machine, SD writer |
| `RocketLogger/PowerPolicy.h` | Transport-off boot and USB-insertion policy |
| `RocketLogger/Config.h` | Pin map, sampling rate, arming thresholds, charger settings |
| `RocketLogger/sketch.yaml` | Pinned, portable Arduino CLI build profile |
| `tools/build.sh` | Optional macOS/Linux build and upload wrapper |
| `tools/*.py` | Optional device test and SD readback helpers |
| `test-results/` | Bench measurements and validation evidence |

## Build dependencies

- Arduino ESP32 core 3.3.12, ESP32S3 Dev Module, 8MB flash, default 8MB partition scheme, USB CDC on boot enabled, no PSRAM.
- Adafruit NeoPixel 1.15.5 and XPowersLib 0.3.3.
- `sketch.yaml` pins the platform and both libraries. Wire, FS, SD_MMC, Preferences (NVS), FreeRTOS, esp_timer, and esp_sleep are supplied by the ESP32 platform. The sensor burst drivers are part of `Logger.cpp`, so no extra accelerometer/barometer library is needed.
- Native Homebrew ctags is used because the bundled Arduino helper is Intel-only on this ARM Mac. The implementation is C++ to avoid automatic sketch prototype generation.

## Build and upload with Arduino CLI

Install [Arduino CLI](https://docs.arduino.cc/arduino-cli/installation/) and run these commands from the repository root. The profile automatically downloads its pinned platform and libraries on the first build; internet access and sufficient disk space are required. Later builds use the profile cache.

```sh
arduino-cli compile --profile cwa_v2 --build-path build/profile RocketLogger
arduino-cli board list
arduino-cli upload --profile cwa_v2 --port YOUR_PORT --input-dir build/profile RocketLogger
arduino-cli monitor --port YOUR_PORT --config baudrate=115200
```

Replace `YOUR_PORT` with the port shown by `board list`, such as `COM5`, `/dev/ttyACM0`, or `/dev/cu.usbmodem11401`. The YAML deliberately does not save this computer's port. The board may appear as Unknown; the profile still selects the ESP32S3 Dev Module explicitly. Close the monitor before uploading.

The profile is the default, so `arduino-cli compile RocketLogger` also selects it. Profile builds use isolated dependency versions rather than arbitrary globally installed libraries. See Arduino's [sketch project file documentation](https://docs.arduino.cc/arduino-cli/sketch-project-file/).

On macOS/Linux, the optional wrapper detects Arduino CLI or the macOS IDE's bundled executable:

```sh
./tools/build.sh
./tools/build.sh upload /dev/cu.usbmodem11401
```

An ARM Mac with the Intel-only ctags error can install native ctags with `brew install ctags`; the wrapper detects it. For the direct CLI command, add `--build-property runtime.tools.ctags.path=/opt/homebrew/bin`. An alternative is to use an Arduino installation with a compatible bundled helper. The wrapper can use a custom executable through `ROCKET_ARDUINO_CLI`.

## Build and upload with Arduino IDE

1. Install Arduino IDE and add `https://espressif.github.io/arduino-esp32/package_esp32_index.json` to Additional Boards Manager URLs.
2. Install **esp32 by Espressif Systems 3.3.12** in Boards Manager.
3. Install **Adafruit NeoPixel 1.15.5** and **XPowersLib 0.3.3** in Library Manager.
4. Open `RocketLogger/RocketLogger.ino`; keep its neighboring files together.
5. Select **ESP32S3 Dev Module**, **USB CDC On Boot: Enabled**, **Flash Size: 8MB**, **Partition Scheme: Default 8MB**, **PSRAM: Disabled**, and the connected USB port. Other options can remain at their board defaults.
6. Upload, then open Serial Monitor at 115200 baud.

The reproducible `sketch.yaml` profile is an Arduino CLI feature. In IDE, select the matching settings and dependency versions manually. Uploading replaces the existing application. If connection fails, hold the play/BOOT button, tap reset, release play, and retry; tap reset after programming if needed.

## Wiring

CWA **v2+**, from https://github.com/domino4com/CWA:

| Function | GPIO |
|---|---:|
| I2C SDA / SCL | 17 / 18 |
| NeoPixel / play button | 39 / 0 |
| SD CLK / CMD | 36 / 37 |
| SD DAT0 / DAT1 / DAT2 / DAT3 | 38 / 34 / 33 / 35 |

Devices: KXTJ3-1057 at 0x0F, SPL06 at 0x77, AXP2101 at 0x34. The SD card mounts in native **4-bit MMC**, requesting 40MHz, without formatting it. A missing device or card produces an orange fault indicator.

## Operation

1. Boot to READY. READY always blinks blue, including when USB is charging. The PMU charge LED shows charging separately; POST can blink purple while charging.
2. **X is the launch axis**, confirmed on the connected board. Hold the rocket upside down (filtered X below −0.75g, gravity magnitude between 0.65g and 1.50g) for 500ms. This starts a new `flight_XXXXXX.csv` and turns the pixel steady red. Recording starts immediately, before launch, so preparation movement is captured.
3. Recording continues while stationary, including any wait on the launch pad. **Press the CWA play button to stop recording.** The logger drains queued rows, flushes and closes the CSV, enters POST, and writes a status TXT with fresh battery telemetry. The pixel then blinks green, or purple if charging. There is no inactivity timeout.
4. To rearm after POST, hold upright for one second, then invert again. Each recording has a new filename, including across restarts.
5. When already READY or POST, a debounced play-button press creates a separate `status_XXXXXX.txt`. While recording it stops the recording first, then creates the report. The serial `t` command can create a report during recording without stopping; those battery fields use the cached pre-flight snapshot, with its time and age included.

Battery blink period varies from 250ms at 0% to 2,950ms at 100%. Unknown/untrustworthy SoC uses the fast period. Armed/recording/closing stays steady red and does not read battery ADC or SoC.

## Acquisition and CSV

The KXTJ3 uses **±16g, 14-bit mode, 800Hz nominal ODR**. The device clock produced about 815 fresh samples/s on the bench. A 1,600Hz configuration was tested but the I2C path only delivered about 1,300 fresh samples/s; use 800Hz for better conversion coverage. `ACCEL_HZ` can be changed to 400, 800, or 1600. Higher motor impulse class alone does not predict peak acceleration; ±16g values can clip during thrust or landing shock. Reassess against the selected motor's thrust curve before changing motors.

The SPL06 runs pressure at 128Hz and temperature at 32Hz with 1x oversampling. It uses the IWB external MEMS temperature source and cached factory compensation. The acquisition path uses checked burst I2C reads and data-ready flags. Pressure/temperature repeat between fresh conversions; their age columns expose this explicitly.

CSV columns:

```
millis,elapsed_ms,micros,seq,x_g,y_g,z_g,temp_C,pressure_Pa,pressure_age_ms,temp_age_ms,dropped_total
```

`millis` is time since boot; `elapsed_ms` is time since this recording began. `micros` exposes sub-millisecond spacing. Acceleration is in g, pressure in Pa, temperature in degrees Celsius. Millisecond/microsecond counters wrap after their normal Arduino intervals; intended flights last seconds/minutes.

A 4,096-record RAM queue separates I2C acquisition from a dedicated SD writer. CSV writes use an 8KB buffer and filesystem sync at least every 500ms when the card is responsive. SD stalls can delay sync and fill the queue; `dropped_total` and sequence discontinuities expose dropped CSV records. Sample timestamp gaps also expose acquisition pauses; zero queue drops does not prove that every sensor conversion was captured. Sudden power removal can lose buffered data. Storage faults stop the recording and show orange.

## Changing settings

Edit `RocketLogger/Config.h`, rebuild, and upload:

| Setting | Current value | Effect |
|---|---|---|
| `ACCEL_HZ` | 800 | Accelerometer ODR; accepted values are 400, 800, 1600 |
| `ARM_AXIS_G` | -0.75 | Negative X threshold for inversion arming |
| `ARM_GRAVITY_MIN_G` / `ARM_GRAVITY_MAX_G` | 0.65 / 1.50 | Static-gravity tolerance for upright/inverted detection, allowing measured sensor bias |
| `ENABLE_CHARGING` | true | Enable PMU charging after initial configuration |
| `CHARGE_CURRENT_MA` | 100 | Supported charger settings in this sketch: 100 or 200mA |

X is selected explicitly in `Logger.cpp`; changing the launch axis also requires updating its orientation checks. Accelerometer range is fixed at ±16g and its conversion factor matches that range. The barometer rates, 500ms arming hold, one-second rearm hold, 500ms sync interval, and queue size are in `Logger.cpp`. Do not change a sensor register setting without also updating its scaling and timing. Defaults are intended for this board and the current C6-2 rocket; reassess acceleration headroom for a future E motor.

## Battery and power

The provided PLA schematic is an AXP2101 design with DCDC1 feeding VCC. Firmware sets and reads back DCDC1 at 3,300mV, leaves it enabled, disables the PMU watchdog, and preserves thermal/voltage protections. It sets a 1-second power-on key hold, long-press shutdown, and a 10-second hardware shutdown backup. The firmware polls the long-press IRQ every 100ms, including in sensor/SD fault. Hold the PMU button for about two seconds, then release it. Software shutdown waits for the release to avoid turning back on because PWRON is still held low. An active recording closes before PMU shutdown. Shutdown latches POWER_OFF, stops acquisition, and turns the NeoPixel off; the PMU charge LED can keep blinking while USB charging continues. A PMU on-status or another power-key press does not restart the application. A saved transport-off latch survives ESP32 resets and battery power cycles. Startup checks that latch before enabling DCDC1 or initializing sensors and SD: a latched battery-only boot shuts down again. USB-powered startup clears the latch and returns to blue READY. If shutdown occurs while USB is already attached, unplug and reinsert USB to start again; its existing presence does not wake it.

PL053450 **3.7V / 900mAh** is the supplied battery (superseding the initial 600mAh description). Charging is configured at a conservative **100mA CC / 4.2V CV**, with 25mA precharge/termination. The supplied 900mA rating is a *discharge* rating; confirm the manufacturer's charge specification before increasing current. The schematic has a fixed resistor instead of a battery thermistor, so the TS input is configured accordingly. The PMU charge LED blinks at 1Hz while charging and stays on otherwise.

The board reported USB insertion as its PMU power-on source. USB supplies the CWA through its LDO and reaches PMU VBUS through the schematic diode. PMU shutdown turns off its rails; the ESP32 can remain powered while USB is attached. Battery-only long-press shutdown was confirmed by the user, followed by a successful restart and USB reconnection. The initial battery telemetry was implausible while charging was disabled; enabling charging restored plausible voltage readings and active charging. The latest measured battery voltage was 3.74V while charging, but the PMU still reported 0% SoC. That percentage is unvalidated and should not yet be used to judge remaining charge or interpret the battery blink rate. SoC is the PMU estimate, not a calibrated model for this cell. USB-only automatic startup and file closure during a battery-only power-key shutdown still need separate checks.

## Serial diagnostics (115200 baud)

| Command | Action |
|---|---|
| `?` | Current state, readings/counters, and ARM eligibility/filtered X/gravity magnitude/hold timer |
| `r` / `s` | Start / stop a bench recording |
| `t` | Create the same status TXT as the play button |
| `l` | List SD files when not recording |
| `d` / `b` | Transfer the last CSV / last status TXT |
| `v` | PMU register and voltage diagnostics outside recording |

States: 0 READY, 1 FLIGHT, 2 CLOSING, 3 POST, 4 FAULT, 5 POWER_OFF. In POWER_OFF, serial `?` reports the saved off latch and checked USB status. The separate USB/LDO supply can still power the ESP32; firmware keeps acquisition and the NeoPixel off. Only a new USB insertion permits startup. Unreadable PMU status is never treated as a wake request. Downloads suppress live console messages and temporarily suspend arming and other serial commands to preserve file transfer integrity. USB writes may wait during a bench download; flight console output remains nonblocking. Normal console writes are nonblocking so an absent or slow USB host cannot stall acquisition. Erasing the full flash also erases the saved transport-off latch; normal firmware uploads preserve it. The play button also serves as the boot strap: holding it while resetting can enter the ESP32 bootloader.

`tools/bench.py` performs a bench recording and collects serial evidence. `tools/readback.py` downloads and validates the last CSV. These require pyserial, installed in the local ignored virtual environment. Test CSV copies and reports are saved in `test-results/`. Diagnostic commands operate on the live board; `r` starts a real SD recording.

Detailed measured results and test limits: [test-results/VALIDATION.md](test-results/VALIDATION.md).

## Status TXT contents

Status reports include uptime, state, current X/Y/Z, temperature, pressure, battery presence, PMU SoC, battery/VBUS/system voltages, USB and charging flags, charge enable setting, battery snapshot time/age, accelerometer ODR/range, acquisition count, dropped CSV rows, I2C error count, maximum observed sample gap, and the last CSV filename. State and battery readings describe the report snapshot. During red recording, an old battery snapshot is expected and intentional.

## Retrieval and analysis

Close recording before removing the SD card. Read CSV/TXT files directly using a card reader, or issue `d`/`b` over serial after recording has closed. These commands retrieve the latest numbered CSV/TXT, including files recovered from the SD card after a restart. Use a card reader to retrieve any older file. Existing SD files are preserved. `seq` begins at zero for each flight, and `dropped_total` counts queue losses within that flight.

For plotting, use `elapsed_ms / 1000` as seconds, plot X/Y/Z in g, and plot `pressure_Pa / 100` in hPa. Sensor readings are uncalibrated beyond the barometer's factory compensation. Physical gesture testing measured upright X approximately +0.88g and inverted X approximately -1.2g; do not assume that an axis offset/scale calibration has been performed. Pressure is absolute pressure, not altitude; altitude conversion requires a reference pressure.

## Optional Python checks

Install Python 3 and pyserial, then run from the repository root:

```sh
python3 -m venv tools/venv
# macOS/Linux:
. tools/venv/bin/activate
# Windows PowerShell instead: tools/venv/Scripts/Activate.ps1
python -m pip install pyserial
python tools/bench.py
python tools/readback.py
```

The Python helpers currently name the development port `/dev/cu.usbmodem11401` explicitly. Change that string to your port before running them on another computer. `bench.py` starts a real recording, confirms it remains active beyond one minute, stops it with serial `s`, and lists files. `status_check.py` tests cached battery status during recording; `no_host_check.py` tests recording with the host serial port closed. `readback.py` saves the latest CSV, checks sequence continuity, finite readings, increasing timestamps and zero queue drops, then writes a JSON report. Checks can overwrite their local result files; keep earlier measurements under distinct names when needed.

## Troubleshooting and remaining checks

| Symptom | Check |
|---|---|
| Orange NeoPixel | Send `?`: fault status reports power/sensor/SD initialization and current sensor ACK/ID probes. `accel_id=35` and `baro_id=10` are expected. Check the connector for any sensor with ACK=0, then reset |
| SD mount fails | Ensure the card uses a filesystem supported by the selected ESP32 build; FAT32 is the practical starting point. Back up before reformatting. Check all six SD signals and board pull-ups |
| No serial output | Enable USB CDC on boot, choose the current USB port, use a data-capable cable, and reopen the monitor after upload/reset |
| Cannot arm | Confirm launch X is positive upright and negative inverted; hold inverted with filtered gravity magnitude between 0.65g and 1.50g for at least 500ms; `?` reports the ARM conditions. After POST, hold upright for one second first |
| Recording stays red while still | Expected: recording continues until Play is pressed, serial `s` is sent, PMU shutdown is requested, or a fault occurs |
| SoC is unknown (`-1`) | Battery absent or battery voltage outside the accepted 2.5–4.35V plausibility range; inspect battery telemetry and connections |
| SoC remains low while charging | Allow charging time; the PMU estimate may need learning. The latest check was approximately 3.74V and 0%, actively charging; the percentage remains unvalidated |
| Upload cannot connect | Close other programs holding the serial port; use the play/BOOT and reset sequence described above |

Physical inversion and the red-to-purple LED transition were observed during follow-up testing. Battery-only long-press shutdown and a subsequent successful restart were confirmed during follow-up testing. Before an actual flight, also verify physical play-button stopping/reporting and power-key file closure during an active recording; see the latest validation record for completed checks. Firmware power settings do not make the separate USB/LDO supply switch off with PMU shutdown. This firmware records payload telemetry and does not control ignition or recovery deployment.

## Latest verified status — 14 October 2026

After the confirmed battery-only long-press shutdown, the board restarted successfully and USB was reconnected. Live checks showed:

- **READY**, with inversion arming eligible; recording was not active.
- Both accelerometer and barometer responding, with **zero I2C errors**. The earlier missing-barometer fault is resolved.
- A new status TXT successfully written to and read back from the SD card.
- Filtered upright X approximately **+0.88g**, temperature **24.1°C**, pressure **985.37hPa**.
- Battery approximately **3.74V**, USB present, actively charging. That snapshot used the earlier purple READY charging indication; the current firmware uses blue READY even while charging, as requested on 15 October.
- PMU SoC still **0%**; this estimate and the SoC-derived blink rate remain to be validated.

Recording starts with the inversion gesture and **does not stop for inactivity**. Play closes it and requests a fresh post-flight status report. Physical Play stopping/reporting and power-key file closure during an active recording remain unverified. A long stationary bench recording was closed via serial; its full CSV download was interrupted by USB disconnection and is not counted as a complete readback pass.

Evidence: [live restart status](test-results/restarted-device-status.log), [SD status readback](test-results/restarted-status-readback.txt), and [validation history](test-results/VALIDATION.md).

## Earlier bench evidence

The one-minute SD readback contained **49,323 rows over 60.499 seconds**, averaging **815.26 records/s**, with consecutive sequence numbers and no queued row losses. Maximum sample gap was **2.653ms**; pressure updated at approximately **127Hz**. Automatic inactivity closing was tested in the earlier firmware but has since been removed: the current firmware requires Play to stop recording. Status TXT creation was tested. The final firmware also recorded with the host serial port closed for eight seconds and passed SD readback. These are bench measurements, not a flight qualification or proof that every sensor conversion is captured. See [validation details](test-results/VALIDATION.md) for the original logs, measured values, and remaining limits.

## Transport shutdown policy — 15 October 2026

Long press and release the AXP2101 key to request shutdown. The intent is saved in ESP32 NVS as soon as the long press is detected, before file closure or the PMU hardware timeout. Software waits for both button release and CSV closure before cutting power. This avoids a held PWRON key immediately requesting another startup.

The firmware removes PMU software/IRQ sleep-wake paths and no longer restarts in response to the PMU simply reporting on. If an unexpected battery-only reset occurs, the saved off request is checked before normal rail/sensor/SD setup and reasserts shutdown. A battery-only shutdown also disables all ESP32 deep-sleep wake sources if the MCU remains powered briefly. There is no timed wake or scheduled restart.

Plugging USB into the core allows startup again: confirmed VBUS clears the saved latch and returns to blue READY, with USB charging still enabled. If USB was connected when shutdown was requested, detach and reattach it. The NeoPixel staying dark while USB is attached is a software OFF state; the USB/LDO supply itself cannot be physically disconnected by the AXP2101. Off-state battery current has not been measured with a meter.

Run the host policy checks with:

```sh
c++ -std=c++11 tools/test_power_policy.cpp -o /tmp/test_power_policy
/tmp/test_power_policy
```

`tools/transport_power_watch.py` is a read-only monitor that reconnects after USB removal during hands-on power testing. It uses the same development serial port as the other Python helpers. See the latest validation record for the physical tests actually completed.
