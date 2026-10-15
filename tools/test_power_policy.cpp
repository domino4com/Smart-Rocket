#include <cassert>
#include <iostream>

#include "../RocketLogger/PowerPolicy.h"
int main() {
  // Battery-only reset or a failed USB reading cannot clear a saved off intent.
  for (int reboot = 0; reboot < 20; ++reboot) {
    assert(!allowLoggerBoot(true, true, false));
    assert(!allowLoggerBoot(true, false, false));
    assert(!allowLoggerBoot(true, false, true));
  }
  assert(allowLoggerBoot(true, true, true));    // confirmed USB permits normal startup
  assert(allowLoggerBoot(false, true, false));  // removing USB during a flight is allowed
  UsbInsertionGate gate;
  gate.arm(true, true);  // shutting down with USB already attached
  for (int i = 0; i < 100; i++) assert(!gate.observe(true, true));
  assert(!gate.observe(false, false));  // I2C failure is not a detach
  assert(!gate.observe(true, true));
  assert(!gate.observe(true, false));  // actual unplug
  assert(!gate.observe(false, true));  // invalid read is not an insertion
  assert(gate.observe(true, true));    // replug
  assert(!gate.observe(true, true));   // one event only
  gate.arm(true, false);               // already off on battery
  assert(gate.observe(true, true));
  std::cout << "PASS: transport latch, invalid USB reads, and fresh-insertion wake policy\n";
}
