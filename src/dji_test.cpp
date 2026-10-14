// Camera-side test: connects to the Osmo Action 5 Pro and logs its status. In the Serial Monitor
// (115200 baud), type a command and press Enter:
//   r   start recording
//   s   stop recording
//   x <type> <cmdset> <cmdid> [payload bytes...]   send any R SDK frame, all values hex,
//       e.g. "x 01 1d 03 00 00 ff 33 00 00 00 00 00" is the same as r, and
//       "x 01 00 11 02 01 00 00" presses QS (toggles pre-rec if QS is set to Start/End Pre-Rec)
// Build with: pio run -e dji-test
#include <Arduino.h>
#include <BLEDevice.h>
#include "dji.h"

String line;  // what has been typed so far

void setup() {
  Serial.begin(115200);
  // Same Bluetooth address as the bridge (main.cpp), in case the camera remembers it.
  const uint8_t base[] = {0x02, 0x60, 0x50, 0x00, 0x00, 0x40};
  esp_base_mac_addr_set(base);
  BLEDevice::init("");
  Serial.printf("bluetooth address %s\n", BLEDevice::getAddress().toString().c_str());
  dji::begin();
}

void command(const String &l) {
  if (l == "r" || l == "s") {
    dji::record(l == "r");
    return;
  }
  uint8_t b[49];  // hex bytes after the command letter
  size_t n = 0;
  const char *s = l.c_str() + 1;
  char *end;
  while (n < sizeof b) {
    long v = strtol(s, &end, 16);
    if (end == s) break;
    b[n++] = v;
    s = end;
  }
  if (l[0] != 'x' || n < 3) Serial.println("commands: r, s, x <type> <cmdset> <cmdid> [payload]");
  else dji::sendRaw(b[0], b[1], b[2], b + 3, n - 3);
}

void loop() {
  dji::loop();

  static dji::Status last;
  dji::Status s = dji::status;
  if (s.updates != last.updates && (s.mode != last.mode || s.state != last.state ||
                                    s.battery != last.battery || s.recordTime != last.recordTime)) {
    if (line.length()) Serial.println();  // don't print into the middle of what is being typed
    Serial.printf("camera: mode 0x%02x, status 0x%02x, battery %u%%, record time %u s%s\n", s.mode,
                  s.state, s.battery, s.recordTime,
                  dji::recording() ? " (RECORDING)" : s.state == 0x05 ? " (PRE-REC)" : "");
    Serial.print(line);  // and show it again below
  }
  last = s;

  // Collect a line ourselves, echoing it: the monitor sends each key as typed without showing
  // it, and Serial's own line reading gives up after a second.
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      line.trim();
      if (line.length()) {
        Serial.println();
        command(line);
      }
      line = "";
    } else if (c == '\b' || c == 0x7f) {  // backspace
      if (line.length()) {
        line.remove(line.length() - 1);
        Serial.print("\b \b");
      }
    } else {
      line += c;
      Serial.print(c);
    }
  }
}
