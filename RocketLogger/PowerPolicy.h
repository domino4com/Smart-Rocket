#pragma once

// A saved transport-off request survives ESP32 resets and battery power cycles.
// A failed PMU read must never be mistaken for permission to start on battery.
inline bool allowLoggerBoot(bool offLatched, bool usbReadValid, bool usbPresent) {
  return !offLatched || (usbReadValid && usbPresent);
}

// When USB already supplies the core at shutdown, it must be removed and
// inserted again before it is allowed to restart the application.
class UsbInsertionGate {
 public:
  void arm(bool valid, bool present) { detached = valid && !present; }
  bool observe(bool valid, bool present) {
    if (!valid) return false;
    if (!present) {
      detached = true;
      return false;
    }
    if (!detached) return false;
    detached = false;
    return true;
  }

 private:
  bool detached = false;
};
