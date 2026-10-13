// Camera-side test: connects to the Osmo Action 5 Pro and logs its status. In the Serial Monitor
// (115200 baud), "r" starts recording and "s" stops it. Build with: pio run -e dji-test
#include <Arduino.h>
#include <BLEDevice.h>
#include "dji.h"

void setup() {
  Serial.begin(115200);
  // Same Bluetooth address as the bridge (main.cpp), in case the camera remembers it.
  const uint8_t base[] = {0x02, 0x60, 0x50, 0x00, 0x00, 0x40};
  esp_base_mac_addr_set(base);
  BLEDevice::init("");
  Serial.printf("bluetooth address %s\n", BLEDevice::getAddress().toString().c_str());
  dji::begin();
}

void loop() {
  dji::loop();

  static dji::Status last;
  dji::Status s = dji::status;
  if (s.updates != last.updates && (s.mode != last.mode || s.state != last.state ||
                                    s.battery != last.battery || s.recordTime != last.recordTime)) {
    Serial.printf("camera: mode 0x%02x, status 0x%02x, battery %u%%, record time %u s%s\n", s.mode,
                  s.state, s.battery, s.recordTime,
                  dji::recording() ? " (RECORDING)" : s.state == 0x05 ? " (PRE-REC)" : "");
  }
  last = s;

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r' || c == 's') {
      Serial.printf("record %s\n", c == 'r' ? "start" : "stop");
      dji::record(c == 'r');
    }
  }
}
