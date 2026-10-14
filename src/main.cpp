// The bridge: shows up on the ELEMNT as a GoPro (Open GoPro BLE service 0xFEA6) and connects to the
// Osmo Action 5 Pro (dji.cpp). The ELEMNT's record button starts and stops the camera, and the
// ELEMNT shows the camera's battery and recording state. Every other command, setting and query
// gets a "success" reply with plausible values. Log at 115200 baud.
// See HANDOFF.md for why a GoPro, and for the radar fallback (commit 7ff56a4).
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include <mutex>
#include "dji.h"

// Name and advertisement copy a real GoPro (Open GoPro test vectors). The ELEMNT lists a GoPro only
// while it advertises "in pairing mode", like a camera put into pairing mode by hand.
const char *kName = "GoPro 0042";  // "GoPro" + last 4 digits of the serial, as cameras do
// ponytail: shown until the camera's first status push; hide the GoPro until then if it misleads.
uint8_t level = 10;
bool statusPush = false;  // the ELEMNT registered for status updates (query 0x53)
bool elemnt = false;      // the ELEMNT is connected
bool rec = false;         // recording, as the ELEMNT is told: see loop()
uint16_t recTime = 0;     // the camera's record time, s, while it records
BLEAdvertisementData advData;  // kept to set it again after the camera's wake-up advertisement
BLECharacteristic *battery, *cmd, *cmdRsp, *set, *setRsp, *qry, *qryRsp;
BLE2902 *batteryCccd;

// Debug: log every GATT read/write the ELEMNT makes, by attribute handle.
// Match handles against the map printed at boot.
void logGatt(esp_gatts_cb_event_t event, esp_gatt_if_t, esp_ble_gatts_cb_param_t *p) {
  if (event == ESP_GATTS_READ_EVT) {
    Serial.printf("GATT read  handle %u\n", p->read.handle);
  } else if (event == ESP_GATTS_WRITE_EVT) {
    Serial.printf("GATT write handle %u:", p->write.handle);
    for (int i = 0; i < p->write.len; i++) Serial.printf(" %02x", p->write.value[i]);
    Serial.println();
  } else if (event == ESP_GATTS_MTU_EVT) {
    Serial.printf("GATT mtu %u\n", p->mtu.mtu);
  } else if (event == ESP_GATTS_CONGEST_EVT) {
    Serial.printf("GATT conn %u %s\n", p->congest.conn_id,
                  p->congest.congested ? "congested" : "no longer congested");
  } else if (event == ESP_GATTS_RESPONSE_EVT && p->rsp.status != ESP_GATT_OK) {
    Serial.printf("GATT response for handle %u FAILED (0x%02x)\n", p->rsp.handle, p->rsp.status);
  }
}

// Debug: connection parameter changes, for both links.
void logGap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
  if (event != ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT) return;
  auto &u = p->update_conn_params;
  Serial.printf("link %s: interval %u.%02u ms, latency %u, timeout %u ms (status %d)\n",
                BLEAddress(u.bda).toString().c_str(), u.conn_int * 5 / 4, u.conn_int * 125 % 100,
                u.latency, u.timeout * 10, u.status);
}

BLEUUID gp(uint16_t id) {  // Open GoPro characteristic UUIDs: b5f9xxxx-aa8d-11e3-9046-0002a5d5c51b
  char s[37];
  snprintf(s, sizeof s, "b5f9%04x-aa8d-11e3-9046-0002a5d5c51b", id);
  return BLEUUID(std::string(s));
}

// Open GoPro framing: a header with the message length, split into 20-byte notifications,
// each continuation packet starting with 0x80. A message that needs more than one packet must use
// the 13-bit header: the ELEMNT only reassembles packets when the first byte has 0x20 or 0x80 set,
// and parses a split message with the 5-bit header as its first packet alone.
// ponytail: fixed 20-byte packets fit the default MTU; use the negotiated MTU if throughput matters.
// notify() drops a packet when the link is congested ("rc=-1" in the log), as in the ELEMNT's burst
// of requests after connecting. Not retried: the ELEMNT asks again for a reply it doesn't get. A
// retry kept the link congested, and then the stack dropped one of its own read or write responses
// (most likely), and the ELEMNT waited for it forever: connected, but it sent nothing more.
std::mutex sending;  // replies go out from the Bluetooth task, status pushes from loop()

void send(BLECharacteristic *c, const uint8_t *msg, size_t n) {
  std::lock_guard<std::mutex> lock(sending);
  uint8_t pkt[20];
  size_t len;
  if (n < sizeof pkt) {  // fits in one packet with the 1-byte header
    pkt[0] = n;
    len = 1;
  } else {
    pkt[0] = 0x20 | (n >> 8);
    pkt[1] = n & 0xff;
    len = 2;
  }
  size_t i = 0;
  for (;;) {
    size_t take = min(n - i, sizeof pkt - len);
    memcpy(pkt + len, msg + i, take);
    i += take;
    c->setValue(pkt, len + take);
    c->notify();
    if (i >= n) break;
    pkt[0] = 0x80;
    len = 1;
  }
  Serial.printf("  reply %s:", c->getUUID().toString().substr(4, 4).c_str());
  for (size_t k = 0; k < n; k++) Serial.printf(" %02x", msg[k]);
  Serial.println();
}

// Status value and its length in bytes. The IDs are the ones the ELEMNT asks for.
uint32_t statusValue(uint8_t id, uint8_t &len) {
  len = 1;
  switch (id) {
    case 1: return 1;                                                  // internal battery present
    case 2: return level > 66 ? 3 : level > 33 ? 2 : level > 10 ? 1 : 0;  // battery bars
    case 10: return rec;                                               // encoding active
    case 33: return 0;                                                 // SD card OK
    case 70: return level;                                             // internal battery %
  }
  len = 4;
  switch (id) {
    case 13: return rec ? recTime : 0;                              // recording duration, s
    case 34: return 999;                                            // photos remaining
    case 35: return 7200;                                           // video remaining, s
    case 96: return 1000;                                           // active preset group: video
    case 97: return 1;                                              // active preset: see kPresets
  }
  len = 1;
  return 0;  // ponytail: everything else reads as 0 until the ELEMNT shows it needs more
}

uint32_t settingValue(uint8_t id) {
  switch (id) {
    case 2: return 1;  // resolution 4K
    case 3: return 8;  // 30 fps
    default: return 0;  // auto-off never, lenses wide, Max Lens Mod off
  }
}

// NotifyPresetStatus, hand-encoded protobuf: one video group (id 1000, icon 0, active preset 1)
// holding one preset (id 1, mode 12 = video, title 1 = "Standard", icon 0).
const uint8_t kPresets[] = {0x0a, 0x11, 0x08, 0xe8, 0x07, 0x12, 0x08, 0x08, 0x01, 0x10,
                            0x0c, 0x18, 0x01, 0x30, 0x00, 0x20, 0x00, 0x38, 0x01};

size_t putValue(uint8_t *out, uint8_t id, uint32_t v, uint8_t len) {  // id, len, big-endian value
  out[0] = id;
  out[1] = len;
  for (int i = 0; i < len; i++) out[2 + i] = v >> (8 * (len - 1 - i));
  return 2 + len;
}

size_t putInt(uint8_t *out, uint32_t v) {  // 4-byte length-prefixed big-endian integer
  out[0] = 4;
  for (int i = 0; i < 4; i++) out[1 + i] = v >> (8 * (3 - i));
  return 5;
}

size_t putStr(uint8_t *out, const char *s) {  // length-prefixed string
  size_t n = strlen(s);
  out[0] = n;
  memcpy(out + 1, s, n);
  return 1 + n;
}

void pushStatus(uint8_t id) {  // unsolicited update, sent as a reply to query 0x53 (0x53 | 0x80)
  if (!statusPush) return;
  uint8_t msg[8] = {0x93, 0x00};
  uint8_t len;
  uint32_t v = statusValue(id, len);
  send(qryRsp, msg, 2 + putValue(msg + 2, id, v, len));
}

// Reply to one ELEMNT request.
// ponytail: single-packet requests only; longer ones are logged (above) but not reassembled.
class GoproWrite : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    std::string v = c->getValue();
    const uint8_t *p = (const uint8_t *)v.data();
    if (v.size() < 2 || (p[0] & 0xe0) != 0) return;  // not a single-packet message
    size_t n = min((size_t)p[0], v.size() - 1);
    const uint8_t *m = p + 1;
    uint8_t out[128];
    size_t k = 0;

    if (m[0] >= 0xf0 && n >= 2) {
      // Protobuf message: feature ID, action ID. The reply's action ID has 0x80 set. Commands get
      // ResponseGeneric{result: SUCCESS} (08 01). Queries get an empty message, except
      // GetPresetStatus (f5 72), which gets the preset list.
      out[k++] = m[0];
      out[k++] = m[1] | 0x80;
      if (c == cmd) {
        out[k++] = 0x08;
        out[k++] = 0x01;
      } else if (m[0] == 0xf5 && m[1] == 0x72) {
        memcpy(out + k, kPresets, sizeof kPresets);
        k += sizeof kPresets;
      }
      Serial.printf("  protobuf feature %02x action %02x\n", m[0], m[1]);
    } else if (c == cmd) {
      out[k++] = m[0];
      out[k++] = 0x00;
      if (m[0] == 0x01 && n >= 3) {
        // Status 10 and 13 follow the camera, not this command (see loop()), so a start the
        // camera refuses (no SD card) does not show as recording.
        Serial.printf("  SHUTTER %s%s\n", m[2] ? "on (start recording)" : "off (stop)",
                      dji::ready() && !dji::asleep() ? "" : ", camera asleep or not connected");
        dji::record(m[2]);
      } else if (m[0] == 0x05) {  // the Sleep button on the GoPro's page in My Sensors
        // It stays there while the camera sleeps (the ELEMNT can't tell), so it also wakes it.
        bool awake = dji::ready() && !dji::asleep();
        Serial.printf("  SLEEP%s\n", awake ? "" : ": the camera sleeps or is away, waking it");
        if (awake) dji::sleep();
        else dji::wake();
      } else if (m[0] == 0x3c) {  // Get Hardware Info: a HERO12 Black, matching the advertisement
        k += putInt(out + k, 62);                 // model number
        k += putStr(out + k, "HERO12 Black");     // model name
        k += putInt(out + k, 0x05);               // board type
        k += putStr(out + k, "H23.01.02.32.00");  // firmware version
        k += putStr(out + k, "C3501324500042");   // serial; last 4 digits match the name
        k += putStr(out + k, "GP24500042");       // Wi-Fi AP SSID
        k += putStr(out + k, "2674f7f65f78");     // Wi-Fi AP MAC
      }
    } else if (c == set) {
      out[k++] = m[0];
      out[k++] = 0x00;
    } else {  // query: ID followed by the setting or status IDs it is about
      uint8_t q = m[0];
      out[k++] = q;
      out[k++] = 0x00;
      if (q == 0x13 || q == 0x53 || q == 0x12 || q == 0x52) {
        for (size_t i = 1; i < n && k + 6 <= sizeof out; i++) {
          uint8_t len = 1;
          uint32_t v = (q & 0x0f) == 0x03 ? statusValue(m[i], len) : settingValue(m[i]);
          k += putValue(out + k, m[i], v, len);
        }
      }
      if (q == 0x53) statusPush = true;
      if (q == 0x73) statusPush = false;
    }
    send(c == cmd ? cmdRsp : c == set ? setRsp : qryRsp, out, k);
  }
};

class Security : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  bool onConfirmPIN(uint32_t) override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t r) override {
    Serial.printf("bonding %s (reason 0x%02x)\n", r.success ? "ok" : "FAILED", r.fail_reason);
  }
};

class BatteryRead : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c) override {
    Serial.printf("  answered battery read with %u%%\n", c->getData()[0]);
  }
};

// These also fire for the camera link, where we are the central (link_role 0).
class Callbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s, esp_ble_gatts_cb_param_t *p) override {
    const esp_gatt_conn_params_t &c = p->connect.conn_params;
    Serial.printf("%s link: interval %u.%02u ms, latency %u, timeout %u ms\n",
                  p->connect.link_role == 1 ? "ELEMNT" : "camera", c.interval * 5 / 4,
                  c.interval * 125 % 100, c.latency, c.timeout * 10);
    if (p->connect.link_role != 1) {
      // Not a client of ours. Without this, every notify() would go to the camera as well.
      s->removePeerDevice(p->connect.conn_id, false);
      return;
    }
    elemnt = true;
    Serial.println("ELEMNT connected");
  }
  void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *p) override {
    // reason: 0x08 supervision timeout, 0x13 the other side closed it, 0x16 we did, 0x22 no answer
    Serial.printf("link %s dropped (reason 0x%02x)\n",
                  BLEAddress(p->disconnect.remote_bda).toString().c_str(), p->disconnect.reason);
    if (p->disconnect.link_role != 1) return;
    elemnt = false;
    statusPush = false;
    Serial.println("ELEMNT disconnected, advertising again");
    if (!dji::waking()) BLEDevice::startAdvertising();  // else loop() does, after the wake-up
  }
};

BLECharacteristic *notifyChr(BLEService *s, uint16_t id) {
  BLECharacteristic *c = s->createCharacteristic(gp(id), BLECharacteristic::PROPERTY_NOTIFY);
  c->addDescriptor(new BLE2902());
  return c;
}

BLECharacteristic *writeChr(BLEService *s, uint16_t id, GoproWrite *cb) {
  BLECharacteristic *c = s->createCharacteristic(
      gp(id), BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  c->setCallbacks(cb);
  return c;
}

void setup() {
  Serial.begin(115200);
  // Fresh Bluetooth address. The ELEMNT keeps a capability store per address that survives
  // "forget sensor", so this board's factory address still carried the cadence capabilities
  // (wheel circumference). Bluetooth address = base + 2. Bump the last byte to start clean again.
  const uint8_t base[] = {0x02, 0x60, 0x50, 0x00, 0x00, 0x40};
  esp_base_mac_addr_set(base);
  BLEDevice::setCustomGattsHandler(logGatt);
  BLEDevice::setCustomGapHandler(logGap);
  BLEDevice::init(kName);
  Serial.printf("bluetooth address %s\n", BLEDevice::getAddress().toString().c_str());

  // The ELEMNT bonds with a GoPro right after connecting. Accept "Just Works" bonding.
  BLEDevice::setSecurityCallbacks(new Security());
  BLESecurity *sec = new BLESecurity();
  sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);
  sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new Callbacks());

  // Open GoPro control and query service: command, setting and query, each with a response.
  BLEService *gopro = server->createService(BLEUUID((uint16_t)0xFEA6), 20);
  GoproWrite *onWrite = new GoproWrite();
  cmd = writeChr(gopro, 0x0072, onWrite);
  cmdRsp = notifyChr(gopro, 0x0073);
  set = writeChr(gopro, 0x0074, onWrite);
  setRsp = notifyChr(gopro, 0x0075);
  qry = writeChr(gopro, 0x0076, onWrite);
  qryRsp = notifyChr(gopro, 0x0077);
  gopro->start();

  // Standard Battery Service: drives the ring around the sensor icon.
  BLEService *bas = server->createService(BLEUUID((uint16_t)0x180F));
  battery = bas->createCharacteristic(BLEUUID((uint16_t)0x2A19),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  batteryCccd = new BLE2902();
  battery->addDescriptor(batteryCccd);
  battery->setValue(&level, 1);
  battery->setCallbacks(new BatteryRead());
  bas->start();

  BLEService *dis = server->createService(BLEUUID((uint16_t)0x180A));
  dis->createCharacteristic(BLEUUID((uint16_t)0x2A29), BLECharacteristic::PROPERTY_READ)
     ->setValue("DIY");                  // manufacturer name
  dis->createCharacteristic(BLEUUID((uint16_t)0x2A24), BLECharacteristic::PROPERTY_READ)
     ->setValue("Osmo Battery Bridge");  // model number
  dis->createCharacteristic(BLEUUID((uint16_t)0x2A26), BLECharacteristic::PROPERTY_READ)
     ->setValue("0.2");                  // firmware revision
  dis->start();

  Serial.printf("handles: cmd %u, cmd rsp %u, set %u, set rsp %u, qry %u, qry rsp %u, battery %u\n",
                cmd->getHandle(), cmdRsp->getHandle(), set->getHandle(), setRsp->getHandle(),
                qry->getHandle(), qryRsp->getHandle(), battery->getHandle());
  Serial.println("(each response handle's subscription descriptor is handle + 1)");

  // Advertisement: 0xFEA6 + GoPro manufacturer data (company 0x02F2). Byte layout from Open GoPro:
  // schema 2; status bits (0x01 powered on, 0x04 pairing mode); camera ID 62 = HERO12 Black
  // (ELEMNT supports HERO9-13 and rejects 60, HERO11 Mini); capabilities; 0; 6-byte ID hash;
  // media offload status.
  advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advData.setPartialServices(BLEUUID((uint16_t)0xFEA6));
  const uint8_t manuf[] = {0xf2, 0x02, 0x02, 0x05, 62, 0x33, 0x00,
                           0xb3, 0xfe, 0x2a, 0x79, 0xdc, 0xeb, 0x0f};
  advData.setManufacturerData(std::string((const char *)manuf, sizeof manuf));
  // Scan response: name, and 0xFEA6 service data = 4 bytes of the Wi-Fi MAC + last 4 serial digits.
  BLEAdvertisementData scanData;
  scanData.setName(kName);
  const uint8_t svc[] = {0xf7, 0xa9, 0x76, 0x88, '0', '0', '4', '2'};
  scanData.setServiceData(BLEUUID((uint16_t)0xFEA6), std::string((const char *)svc, sizeof svc));
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  BLEDevice::startAdvertising();
  Serial.printf("advertising as '%s'\n", kName);

  dji::begin();  // the camera side: scans, connects and pairs from dji::loop()
}

void loop() {
  dji::loop();

  static bool wasWaking = false;
  if (wasWaking && !dji::waking()) {  // the camera's wake-up advertisement is over: ours again
    BLEDevice::getAdvertising()->setAdvertisementData(advData);
    if (!elemnt) BLEDevice::startAdvertising();  // while connected, the ELEMNT needs none
  }
  wasWaking = dji::waking();

  // A stop counts once it has held for kStopHold. The camera's pushes come bunched on the slow link,
  // and after one stop they read stopped, recording, stopped within 120 ms: the ELEMNT's indicator
  // flickered. The record time freezes meanwhile, so pre-rec's buffered seconds don't show.
  // ponytail: raise kStopHold if it still flickers; it delays the stop on the ELEMNT by as much.
  const uint32_t kStopHold = 500;
  static uint32_t lastRec = 0;  // millis() when the camera last showed a recording
  static bool raw = false;      // dji::recording() on the last pass
  static uint16_t lastTime = 0;
  bool was = raw;
  raw = dji::recording();
  if (raw) {
    lastRec = millis();
    recTime = dji::status.recordTime;
    if (!was && rec) Serial.println("camera recording again right after a stop: ignored the stop");
  }
  bool now = raw || (rec && millis() - lastRec < kStopHold);
  // The ELEMNT never polls status 13; its timer moves only with these pushes. A GoPro sends a
  // registered status whenever it changes, so this one once a second while recording.
  if (now != rec) {
    rec = now;
    Serial.printf("camera %s recording\n", rec ? "started" : "stopped");
    pushStatus(10);
    pushStatus(13);
  } else if (rec && recTime != lastTime) {
    pushStatus(13);
  }
  lastTime = recTime;

  uint8_t b = dji::status.battery;  // 0xff until the camera's first status push
  if (b <= 100 && b != level) {
    level = b;
    battery->setValue(&level, 1);
    // The ELEMNT never subscribes to 0x2A19 but accepts a notification anyway.
    // Faking our side of the subscription lets notify() send.
    batteryCccd->setNotifications(true);
    battery->notify();
    pushStatus(70);
    Serial.printf("camera battery %u%% (notified)\n", level);
  }
}
