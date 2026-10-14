#include "dji.h"
#include <Arduino.h>
#include <BLEDevice.h>

namespace dji {

Status status = {0, 0, 0xff, 0, 0};

namespace {

// Our identity in the connection request. The camera remembers it once the user taps Accept, and
// asks again if either value changes, so keep both constant.
const uint32_t kDeviceId = 0x0b0b0042;                         // arbitrary
const uint8_t kMac[6] = {0x02, 0x60, 0x50, 0x00, 0x00, 0x42};  // the bridge's Bluetooth address

enum State { kIdle, kScanning, kFound, kPairing, kReady, kRejected };
volatile State state = kIdle;
BLEAdvertisedDevice *camera;
BLEClient *client;
BLERemoteCharacteristic *tx;  // 0xFFF5: frames to the camera
uint16_t seq = 1;
uint32_t pairStart;

// DJI's CRCs: reflected, init 0x3AA3, no final XOR. poly 0xA001 = CRC16, 0xEDB88320 = CRC32.
uint32_t crc(const uint8_t *p, size_t n, uint32_t poly) {
  uint32_t c = 0x3aa3;
  while (n--) {
    c ^= *p++;
    for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ poly : c >> 1;
  }
  return c;
}

void dump(const char *tag, const uint8_t *f, size_t n) {
  Serial.printf("  dji %s:", tag);
  for (size_t i = 0; i < n; i++) Serial.printf(" %02x", f[i]);
  Serial.println();
}

// One R SDK frame. type: 0x00 no reply wanted, 0x01/0x02 reply wanted, 0x20 this is a response.
size_t build(uint8_t *f, uint8_t type, uint8_t set, uint8_t id, const uint8_t *p, size_t n,
             uint16_t s) {
  size_t len = 18 + n;
  memset(f, 0, 14);
  f[0] = 0xaa;
  f[1] = len;
  f[2] = len >> 8;
  f[3] = type;
  f[8] = s;
  f[9] = s >> 8;
  uint32_t c = crc(f, 10, 0xa001);
  f[10] = c;
  f[11] = c >> 8;
  f[12] = set;
  f[13] = id;
  memcpy(f + 14, p, n);
  c = crc(f, 14 + n, 0xedb88320);
  memcpy(f + 14 + n, &c, 4);  // little-endian, like the ESP32
  return len;
}

// One frame per write; Write Request if we want a reply. Calls the stack directly, without
// BLERemoteCharacteristic::writeValue(): that blocks until the write's completion event, which
// deadlocks in the notify callback and hung in loop() while the camera flooded us with
// notifications. This call only queues the write, so it works from either task.
uint32_t lastKey;  // type and command of the last frame from the camera, see onNotify()

void send(uint8_t type, uint8_t set, uint8_t id, const uint8_t *p, size_t n, uint16_t s) {
  uint8_t f[64];
  size_t len = build(f, type, set, id, p, n, s);
  dump("tx", f, len);
  lastKey = 0;  // always log the reply
  esp_ble_gattc_write_char(client->getGattcIf(), client->getConnId(), tx->getHandle(), len, f,
                           type & 0x1f ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
                           ESP_GATT_AUTH_REQ_NONE);
}

// The camera's own connection request (not a response) with verify_mode 2 carries the verdict.
// It repeats it about 50 times a second until it gets our response, so answer at once.
void onVerdict(const uint8_t *f, const uint8_t *p) {
  uint8_t a[9] = {};  // device_id, ret_code 0, reserved[0] = camera number 0
  memcpy(a, &kDeviceId, 4);
  send(0x20, 0x00, 0x19, a, sizeof a, f[8] | f[9] << 8);  // a response reuses the camera's SEQ
  if (state != kPairing) return;
  if (p[27] != 0) {
    Serial.println("camera: pairing rejected; reset the bridge to try again");
    state = kRejected;
    client->disconnect();
    return;
  }
  const uint8_t sub[] = {3, 20, 0, 0, 0, 0};  // push_mode 3 (periodic + on change), 20 = 2 Hz
  send(0x00, 0x1d, 0x05, sub, sizeof sub, seq++);
  state = kReady;
  Serial.println("camera: paired, status subscribed");
}

void onNotify(BLERemoteCharacteristic *, uint8_t *f, size_t len, bool) {
  if (len < 14 || f[0] != 0xaa) return;  // DUML frames (0x55) share this characteristic
  // Log a frame only when it differs in type and command from the one before, so the camera's
  // repeats don't stall this (the Bluetooth) task on Serial.
  uint32_t key = f[3] << 16 | f[12] << 8 | f[13];
  bool repeat = key == lastKey;
  lastKey = key;
  uint32_t c32;
  memcpy(&c32, f + len - 4, 4);
  if (len < 18 || (f[1] | (f[2] & 3) << 8) != len || crc(f, 10, 0xa001) != (f[10] | f[11] << 8) ||
      crc(f, len - 4, 0xedb88320) != c32) {
    if (!repeat) dump("bad frame", f, len);  // truncated by a small MTU, most likely
    return;
  }
  const uint8_t *p = f + 14;
  size_t n = len - 18;
  uint16_t cmd = f[12] << 8 | f[13];
  if (cmd == 0x1d02 && n >= 38) {  // status push, 2 Hz: too frequent to log
    status.mode = p[0];
    status.state = p[1];
    status.recordTime = p[5] | p[6] << 8;
    status.battery = p[37];
    status.updates++;
    return;
  }
  if (cmd == 0x0019 && !(f[3] & 0x20) && n >= 29 && p[26] == 2) onVerdict(f, p);
  if (!repeat) dump("rx", f, len);
}

class Found : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    std::string m = d.getManufacturerData();  // DJI: AA 08 <model, 2> FA <MAC, 6>
    if (state != kScanning || m.size() < 5 || (uint8_t)m[0] != 0xaa || m[1] != 0x08 ||
        (uint8_t)m[4] != 0xfa)
      return;
    BLEDevice::getScan()->stop();
    delete camera;
    camera = new BLEAdvertisedDevice(d);
    state = kFound;
  }
};

class Link : public BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override {
    Serial.println("camera disconnected");
    if (state != kRejected) state = kIdle;
  }
};

void connect() {
  Serial.printf("camera found at %s, connecting\n", camera->getAddress().toString().c_str());
  BLEDevice::getScan()->clearResults();
  state = kPairing;
  if (!client->connect(camera)) {
    Serial.println("camera: connect failed");
    state = kIdle;
    return;
  }
  BLERemoteService *svc = client->getService(BLEUUID((uint16_t)0xfff0));
  BLERemoteCharacteristic *rx = svc ? svc->getCharacteristic(BLEUUID((uint16_t)0xfff4)) : nullptr;
  tx = svc ? svc->getCharacteristic(BLEUUID((uint16_t)0xfff5)) : nullptr;
  if (!rx || !tx) {
    Serial.println("camera: no 0xFFF0 service");
    client->disconnect();
    return;
  }
  // BLEClient asks for the MTU on its connect event, but on the first connection after boot it
  // doesn't know its connection ID yet and skips the request (the log showed MTU 23). Ask again.
  client->setMTU(500);
  rx->registerForNotify(onNotify);
  delay(2000);  // DJI's demo waits this long after enabling notifications
  Serial.printf("camera: MTU %u\n", client->getMTU());  // below 54 the request below is split

  // Connection request: device_id, mac_len, mac (16), fw_version 0 (no update prompts), conidx 0,
  // verify_mode 0 (the camera asks the user only if it does not know us), verify_data, reserved.
  uint8_t p[33] = {};
  memcpy(p, &kDeviceId, 4);
  p[4] = sizeof kMac;
  memcpy(p + 5, kMac, sizeof kMac);
  uint16_t code = random(10000);
  p[27] = code;
  p[28] = code >> 8;
  pairStart = millis();
  send(0x02, 0x00, 0x19, p, sizeof p, seq++);
  Serial.println("camera: tap Accept on the camera if it asks");
}

}  // namespace

void begin() {
  // Self-test against the subscribe frame in DJI_PROTOCOL.md.
  const uint8_t want[] = {0xaa, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x51, 0xee,
                          0x1d, 0x05, 0x03, 0x14, 0x00, 0x00, 0x00, 0x00, 0x30, 0xfc, 0xe7, 0x44};
  const uint8_t sub[] = {3, 20, 0, 0, 0, 0};
  uint8_t f[sizeof want];
  build(f, 0x00, 0x1d, 0x05, sub, sizeof sub, 2);
  if (memcmp(f, want, sizeof want)) Serial.println("dji: frame self-test FAILED");

  BLEDevice::setMTU(500);  // the camera needs one whole frame per write and per notification
  client = BLEDevice::createClient();
  client->setClientCallbacks(new Link());
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new Found());
  scan->setActiveScan(true);
}

// ponytail: a dead link is noticed only via the BLE supervision timeout; add a "no 1D02 for 8 s"
// check if the camera turns out to stay connected but silent. Sleep and wake-up are not handled.
void loop() {
  if (state == kIdle) {
    Serial.println("camera: scanning");
    state = kScanning;
    BLEDevice::getScan()->start(0, nullptr, false);  // until Found stops it
  } else if (state == kFound) {
    connect();
  } else if (state == kPairing && millis() - pairStart > 30000) {
    Serial.println("camera: no pairing answer in 30 s, reconnecting");
    pairStart = millis();
    client->disconnect();  // onDisconnect goes back to scanning
  }
}

bool ready() { return state == kReady; }

// Status 0x03 with the record time running. Not 0x05: that is the armed pre-rec buffer, where the
// record time counts buffered seconds. The camera also shows 0x03 with time 0 for one push when it
// starts pre-rec and when a recording stops, so time 0 doesn't count (a real start shows it ~1 s late).
// ponytail: 0x03 also means "taking a photo", so photo mode (0x05) is excluded; other photo-type
// modes would read as recording if their record time runs.
bool recording() {
  return ready() && status.state == 0x03 && status.recordTime > 0 && status.mode != 0x05;
}

void record(bool start) {
  if (!ready()) return;
  // device_id as DJI's demo sends it (the camera seems not to check it), record_ctrl, reserved.
  const uint8_t p[9] = {0x00, 0x00, 0xff, 0x33, (uint8_t)!start};
  send(0x01, 0x1d, 0x03, p, sizeof p, seq++);
}

void sendRaw(uint8_t type, uint8_t set, uint8_t id, const uint8_t *p, size_t n) {
  if (ready() && n <= 46) send(type, set, id, p, n, seq++);  // 46 = room in send()'s frame buffer
}

}  // namespace dji
