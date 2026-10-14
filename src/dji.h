// DJI Osmo Action 5 Pro over BLE, using DJI's R SDK protocol (see DJI_PROTOCOL.md).
// The bridge is the central: it scans for the camera, connects, pairs at the protocol level
// (the user taps Accept on the camera the first time) and subscribes to the status push.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace dji {

// Latest status push (1D02). Written from the Bluetooth task.
// ponytail: plain fields, no lock; a torn read only shows one stale byte until the next push.
struct Status {
  uint8_t mode;         // camera_mode: 0x01 video, 0x05 photo, ...
  uint8_t state;        // camera_status: 0x03 photo or recording, 0x05 pre-recording
  uint8_t battery;      // percent; 0xff until the first push
  uint16_t recordTime;  // s
  uint32_t updates;     // counts pushes, so the caller can spot a new one
};
extern Status status;

void begin();  // after BLEDevice::init()
void loop();   // from loop(): scans, connects, pairs and reconnects
bool ready();  // paired and subscribed to status
bool recording();
void record(bool start);
void sendRaw(uint8_t type, uint8_t set, uint8_t id, const uint8_t *p, size_t n);  // for experiments

}  // namespace dji
