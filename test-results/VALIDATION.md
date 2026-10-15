# Smart Rocket device validation

Bench session: **14 October 2026**. Hardware: CWA v2+ ESP32-S3 FN8, 8MB flash, KXTJ3-1057 at 0x0F, SPL06 at 0x77, AXP2101 at 0x34, and a native four-bit SD card. No launch was performed.

## Current verified state

After the user confirmed battery-only long-press shutdown, the board restarted and USB was reconnected. The latest live check showed:

- READY (state 0), inversion arming eligible, both sensors responding, zero I2C errors.
- A status TXT successfully created on the SD card and read back.
- Upright filtered X around +0.88g, temperature around 24.1°C, pressure around 98,537Pa.
- Battery around 3,742mV, USB present, actively charging.
- PMU SoC still 0%; this percentage and the resulting battery blink rate remain unvalidated for the battery model.

Evidence: [live status](restarted-device-status.log) and [status TXT readback](restarted-status-readback.txt). These are saved snapshots, not live measurements.

## Sensor acquisition and complete SD readbacks

The earlier one-minute bench CSV was downloaded from the SD card and validated:

| Measurement | Result |
|---|---:|
| Rows | 49,323 |
| Duration | 60.499s |
| Average accelerometer record rate | 815.259Hz at nominal 800Hz ODR |
| Largest sample timestamp gap | 2,653µs |
| Queue drops | 0 |
| Sequence continuity | Passed |
| Finite sensor values | Passed |
| Maximum pressure age | 9ms |

Pressure update counters indicated about 127Hz. Acceleration was recorded at ±16g, 14-bit resolution. Evidence: [summary](long-readback.json). [Example CSV](example-flight.csv) contains the first 100 rows from that validated recording; the full raw capture is retained locally.

The firmware also recorded with the host serial connection closed for eight seconds, while USB power stayed connected. SD readback passed: 7,350 rows over 9.013s, 815.38 records/s, consecutive sequence numbers, zero queue losses, maximum sample gap 2,227µs. Evidence: [summary](no-host-readback.json) and [serial log](no-host-check.log). This was a serial-reader test, not a USB power-cycle or battery-only recording test.

A 1,600Hz ODR was evaluated but delivered only about 1,300 fresh readings/s through this I2C acquisition path. The current default is 800Hz. Zero queue drops does not prove that every physical sensor conversion was captured; timestamp gaps expose acquisition pauses.

## Physical inversion and power key

Physical inversion initially failed because the sensor measured approximately −1.23g inverted versus +0.88g upright, exceeding the original static-gravity limit. The accepted filtered magnitude is now 0.65–1.50g, with X below −0.75g for 500ms. The user repeated the gesture and confirmed a red NeoPixel; serial output confirmed FLIGHT and an open CSV.

The user subsequently confirmed battery-only long-press shutdown and a successful restart. Firmware polls the power key every 100ms even in a sensor/SD fault, closes a recording before shutdown, and latches POWER_OFF. A ten-second hardware shutdown backup is configured. USB's separate LDO can still power the ESP32 while PMU rails are off.

The earlier barometer connection fault was resolved by the latest successful live check. Startup now allows sensor power-up time, and fault status exposes sensor ACK/ID probes. Latest numbered CSV/TXT filenames are recovered after a restart for diagnostic downloading.

## Manual stopping and status files

Current firmware has **no inactivity stop**. Play requests CSV closure, then writes a status TXT using fresh battery telemetry after recording ends. A stop request is retained if an SD backlog temporarily prevents enqueueing it.

A stationary bench run stayed active for more than three minutes, then was closed using serial `s`. The writer reported 183,270 rows, zero queue losses, zero I2C errors, and maximum SD-operation latency 866,430µs. Evidence: [closure log](manual-stop-serial-close.log). USB disconnected during its large CSV download, so it is **not** counted as a complete CSV-readback pass.

A status TXT written during recording using serial `t` was previously validated to contain the cached pre-flight battery snapshot and its age. Serial `t` does not stop recording; the physical Play button does.

## Remaining checks

- Physical CWA Play stopping and automatic post-stop TXT creation.
- Power-key shutdown and complete SD file closure during an active battery-only recording.
- USB-only automatic startup under controlled power-cycle conditions.
- PMU charge-LED timing and visible colours beyond the observed red/purple transition.
- Battery SoC calibration/accuracy and accelerometer offset/scale calibration.
- Flight performance with the chosen motor, shock, vibration, and SD card.

DCDC1 was configured and read back at 3,300mV; no independent multimeter measurement of the rail was made. This is bench validation, not flight qualification.

## Transport-off update — 15 October 2026

The user reported delayed startup after long-press shutdown. The previous code explicitly restarted the ESP32 if PMU on-status reappeared; this restart path was removed. Shutdown intent is now stored in NVS when the long press is detected. Software waits for key release and file closure before cutting power. Before enabling DCDC1 or initializing sensors/SD, a battery-only reboot with that saved intent reasserts shutdown. Checked USB presence is required to clear it. If USB is already attached during shutdown, only its subsequent removal and insertion wakes the existing OFF session. Unreadable PMU status is not treated as a wake event.

Power-down removes PMU software/IRQ sleep-wake paths and disables all ESP32 deep-sleep wake sources if the MCU remains powered on battery. READY now blinks blue even while charging; the PMU charge LED remains available for charging indication. The existing hardware shutdown backup is retained.

Host C++ tests passed for repeated latched battery boots, invalid USB status reads, removal/reinsertion gating, and no wake from an already-attached USB source. The pinned Arduino build compiled and uploaded with hash verification. Live startup was READY with both sensors working, zero I2C errors, and off_latched=0. Battery telemetry was approximately 4.13V and 93–94%, actively charging; unlike the earlier snapshot, the PMU is now returning a nonzero SoC estimate. Percentage accuracy is still not calibrated.

The delayed-restart scenario, battery-only off dwell, and USB wake after an off dwell still require physical confirmation for this updated firmware. Off-state current has not been measured. Do not count the older physical shutdown test as verification of the new persistent latch.

Reproduce the policy checks with `c++ -std=c++11 tools/test_power_policy.cpp -o /tmp/test_power_policy && /tmp/test_power_policy`.
