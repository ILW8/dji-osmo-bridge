#include "dji.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <Preferences.h>

namespace dji {

Status status = {0, 0, 0, 0xff, 0, 0};

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
uint32_t lastPush;      // millis() of the last status push
uint32_t pendingStart;  // millis() of a start waiting for the camera to wake up; 0 if none
uint32_t wakeStart;     // millis() when the wake-up advertisement started; 0 if none
uint32_t arming;        // millis() when QS was pressed to arm pre-rec again; 0 if not waiting
bool preRec;            // pre-rec was armed the last time the camera was idle
bool rearm;             // pre-rec was armed when the camera fell asleep: arm it again once back
uint8_t camMac[6];      // the camera's address, for the wake-up advertisement; kept in NVS
bool known;             // camMac is set
uint32_t lastTry;       // millis() of the last connection attempt; 0 once one gets as far as pairing
volatile bool sleepy;   // that attempt got a link but no answers: the camera sleeps
bool answered;          // a valid frame came on this link: the camera is awake
esp_timer_handle_t watchdog;

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
  lastTry = 0;
  status.powerMode = 0;  // it answered, so it is awake
  lastPush = millis();
  // A slow link from now on: 2 pushes a second need little, and the ELEMNT link shares the radio.
  // Its notifications were dropped as congested (rc=-1) only once the camera was connected too.
  esp_ble_conn_update_params_t cp = {};
  memcpy(cp.bda, camMac, 6);
  cp.min_int = 80;   // 100 ms
  cp.max_int = 100;  // 125 ms
  cp.timeout = 400;  // 4 s
  esp_ble_gap_update_conn_params(&cp);
  state = kReady;
  Serial.println("camera: paired, status subscribed");
}

void pressQs() {  // short press of the QS button; set on the camera to Start/End Pre-Rec, it toggles
  const uint8_t k[] = {0x02, 0x01, 0x00, 0x00};  // key_code QS, mode 1 (event), key_value 0 (short)
  send(0x01, 0x00, 0x11, k, sizeof k, seq++);
}

// For each status push while the camera is awake: finish what waited for it to wake up. The wake-up
// drops pre-rec, so arm it again first (from idle only, as QS toggles), then start a pending recording:
// a start with pre-rec armed keeps the buffer.
// ponytail: QS arms pre-rec only with the camera's QS button set to Start/End Pre-Rec. Pressed only
// when pre-rec was armed before the sleep, so that setting is in use.
void back() {
  if (rearm && (status.state == 0x01 || status.state == 0x05)) {
    rearm = false;
    if (status.state == 0x01) {
      Serial.println("camera: arming pre-rec again");
      pressQs();
      arming = millis();  // never 0: we're past boot
    }
  }
  if (arming && (status.state == 0x05 || millis() - arming > 3000)) arming = 0;
  if (pendingStart && !rearm && !arming) {
    bool fresh = millis() - pendingStart < 30000;
    pendingStart = 0;
    Serial.println(fresh ? "camera: starting the recording" : "camera: back too late to record");
    if (fresh) record(true);
  }
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
  answered = true;
  const uint8_t *p = f + 14;
  size_t n = len - 18;
  uint16_t cmd = f[12] << 8 | f[13];
  if (cmd == 0x1d02 && n >= 38) {  // status push, 2 Hz: too frequent to log
    status.mode = p[0];
    status.state = p[1];
    status.recordTime = p[5] | p[6] << 8;
    if (p[28] != status.powerMode) Serial.printf("camera: power mode %u\n", p[28]);
    status.powerMode = p[28];
    status.battery = p[37];
    status.updates++;
    lastPush = millis();
    if (p[28] == 0 && (p[1] == 0x01 || p[1] == 0x05)) preRec = p[1] == 0x05;
    if (state == kReady && !asleep()) back();
    return;
  }
  if (cmd == 0x0019 && !(f[3] & 0x20) && n >= 29 && p[26] == 2) onVerdict(f, p);
  if (!repeat) dump("rx", f, len);
}

class Found : public BLEAdvertisedDeviceCallbacks {
  // The scan reports each address once (BLEScan and the controller both drop repeats), so a camera
  // whose first advertisement fails the DJI check is never seen again in that scan. After a bridge
  // reset, the bridge once scanned for 60 s without finding the awake camera. So the camera we
  // know counts by its address, whatever it advertises.
  void onResult(BLEAdvertisedDevice d) override {
    if (state != kScanning) return;
    std::string m = d.getManufacturerData();  // DJI: AA 08 <model, 2> FA <MAC, 6>
    bool dji = m.size() >= 5 && (uint8_t)m[0] == 0xaa && m[1] == 0x08 && (uint8_t)m[4] == 0xfa;
    if (!dji && !(known && !memcmp(*d.getAddress().getNative(), camMac, 6))) return;
    if (!dji) Serial.printf("camera: its advertisement has no DJI data (%u bytes)\n",
                            d.getPayloadLength());
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

// A sleeping camera keeps advertising and takes the connection, but answers nothing. BLEClient
// then waits for its services forever: loop() hung for 49 s, and with it the end of the wake-up
// advertisement. The camera woke up meanwhile, but the stuck link never recovered. Disconnecting
// ends the wait; loop() holds off the next attempt.
// Not client->disconnect(): closing during discovery made Bluedroid run the queued MTU request,
// which fails while discovery's own request is unanswered, and bta_gattc_cfg_mtu then retried the
// next queued MTU request forever (stack overflow in BTU_TASK). Once the link is gone, Bluedroid no
// longer sends queued requests, so drop the link itself.
void stalled(void *) {
  sleepy = true;
  Serial.println("camera: connected, but it answers nothing (asleep?), disconnecting");
  esp_ble_gap_disconnect(camMac);
}

void connect() {
  Serial.printf("camera found at %s, connecting\n", camera->getAddress().toString().c_str());
  BLEDevice::getScan()->clearResults();
  const uint8_t *mac = *camera->getAddress().getNative();
  if (!known || memcmp(camMac, mac, 6)) {  // a new camera: remember it, to wake it after a reboot
    memcpy(camMac, mac, 6);
    known = true;
    Preferences nv;
    nv.begin("dji");
    nv.putBytes("mac", camMac, 6);
    nv.end();
  }
  state = kPairing;
  lastTry = pairStart = millis();
  sleepy = answered = false;
  if (!client->connect(camera)) {
    Serial.println("camera: connect failed");
    state = kIdle;
    return;
  }
  esp_timer_start_once(watchdog, 5000000);  // 5 s for the camera to answer, see stalled()
  BLERemoteService *svc = client->getService(BLEUUID((uint16_t)0xfff0));
  BLERemoteCharacteristic *rx = svc ? svc->getCharacteristic(BLEUUID((uint16_t)0xfff4)) : nullptr;
  tx = svc ? svc->getCharacteristic(BLEUUID((uint16_t)0xfff5)) : nullptr;
  if (!rx || !tx) {
    esp_timer_stop(watchdog);
    if (!sleepy) Serial.println("camera: no 0xFFF0 service");
    client->disconnect();
    return;
  }
  // BLEClient asks for the MTU on its connect event, but on the first connection after boot it
  // doesn't know its connection ID yet and skips the request (the log showed MTU 23). Ask again.
  client->setMTU(500);
  rx->registerForNotify(onNotify);
  esp_timer_stop(watchdog);
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
  pairStart = millis();  // again: the timeouts in loop() count from the request
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

  Preferences nv;  // the camera from an earlier boot: one that sleeps since then can still be woken
  nv.begin("dji", true);
  known = nv.getBytes("mac", camMac, 6) == 6;
  nv.end();
  if (known) Serial.printf("camera: last seen at %s\n", BLEAddress(camMac).toString().c_str());

  esp_timer_create_args_t t = {};
  t.callback = stalled;
  esp_timer_create(&t, &watchdog);

  BLEDevice::setMTU(500);  // the camera needs one whole frame per write and per notification
  client = BLEDevice::createClient();
  client->setClientCallbacks(new Link());
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new Found());
  scan->setActiveScan(true);
}

void loop() {
  if (wakeStart && ((ready() && !asleep()) || millis() - wakeStart > 30000)) {
    BLEDevice::getAdvertising()->stop();
    wakeStart = 0;
    Serial.println(ready() && !asleep() ? "camera: woken up" : "camera: did not wake up in 30 s");
  }
  static bool wasAsleep = false;
  if (ready() && asleep() != wasAsleep) {
    wasAsleep = !wasAsleep;
    if (wasAsleep) {
      rearm = preRec;
      Serial.printf("camera: asleep (power mode %u, last status %lu ms ago%s)\n", status.powerMode,
                    millis() - lastPush, rearm ? ", pre-rec was armed" : "");
    } else {
      Serial.println("camera: awake");
    }
  }

  // After a failed attempt, wait before the next: 2 s (it spun at 4 Hz once, each link dropped as
  // it came up), or 30 s if the camera sleeps, as each try holds a dead link for 5-8 s. While it is
  // being woken: from 1.5 s into the wake-up (it was back within 1-2 s), and soon after each try.
  // ponytail: a camera switched on by hand during the 30 s is found when they're over.
  uint32_t w = wakeStart, t = lastTry, now = millis();  // now last: never before w (other core)
  bool hold = w ? now - w < 1500 || (t && now - t < 1500)
                : t && now - t < (sleepy ? 30000 : 2000);
  if (state == kIdle && !hold) {
    Serial.println("camera: scanning");
    state = kScanning;
    BLEDevice::getScan()->start(0, nullptr, false);  // until Found stops it
  } else if (state == kFound) {
    connect();
  } else if (state == kPairing && millis() - pairStart > (answered ? 30000 : 5000)) {
    // An awake camera answers the connection request within 0.1 s, also before the user taps
    // Accept. A sleeping one may answer everything else on the link, but not that. (The request
    // went out 4 s into one attempt, where connecting took 1.3 s: so count from the request.)
    sleepy = !answered;
    Serial.println(answered ? "camera: no pairing answer in 30 s, reconnecting"
                            : "camera: no answer to the connection request (asleep?), disconnecting");
    pairStart = millis();
    // Like stalled(): a GATT close with a request unanswered is risky in Bluedroid.
    esp_ble_gap_disconnect(camMac);  // onDisconnect goes back to scanning
  }
}

bool ready() { return state == kReady; }

// A sleeping camera keeps the link up and goes quiet; the last pushes before that may show power
// mode 3. The link drops when it wakes up. A lost link is noticed by the BLE supervision timeout.
// ponytail: a camera that hangs but stays connected also reads as asleep, and a wake-up advertisement
// won't help it. Disconnect after a longer silence if that ever shows up.
bool asleep() { return ready() && (status.powerMode == 3 || millis() - lastPush > 3000); }

// Status 0x03 with the record time running. Not 0x05: that is the armed pre-rec buffer, where the
// record time counts buffered seconds. The camera also shows 0x03 with time 0 for one push when it
// starts pre-rec and when a recording stops, so time 0 doesn't count (a real start shows it ~1 s late).
// ponytail: 0x03 also means "taking a photo", so photo mode (0x05) is excluded; other photo-type
// modes would read as recording if their record time runs.
bool recording() {
  return ready() && !asleep() && status.state == 0x03 && status.recordTime > 0 &&
         status.mode != 0x05;
}

void record(bool start) {
  pendingStart = 0;
  if (ready() && !asleep()) {
    // device_id as DJI's demo sends it (the camera seems not to check it), record_ctrl, reserved.
    const uint8_t p[9] = {0x00, 0x00, 0xff, 0x33, (uint8_t)!start};
    send(0x01, 0x1d, 0x03, p, sizeof p, seq++);
  } else if (start) {  // started from onNotify once the camera is back
    pendingStart = millis();
    wake();
  }
}

void sleep() {
  if (!ready() || asleep()) return;
  const uint8_t p[] = {3};  // power_mode 3 = sleep
  send(0x01, 0x00, 0x1a, p, sizeof p, seq++);
  status.powerMode = 3;  // send nothing more: DJI says a sleeping camera can't take data
}

// DJI's wake-up advertisement, in place of the caller's until the camera is back (30 s at most;
// one 3 s round, as other remotes use, woke it from our sleep command but not from the power
// button): manufacturer data "WKP" + the camera's address in reverse, and no flags. The camera
// wants connectable ADV_IND; BLEAdvertising's other defaults fit too (20-40 ms, and our public
// address, which the camera knows). DJI says the camera then drops the link; ours kept it.
void wake() {
  if ((ready() && !asleep()) || wakeStart) return;
  if (!known) {
    Serial.println("camera: never seen, so it can't be woken");
    return;
  }
  uint8_t a[11] = {10, 0xff, 'W', 'K', 'P'};
  for (int i = 0; i < 6; i++) a[5 + i] = camMac[5 - i];
  BLEAdvertisementData d;
  d.addData(std::string((const char *)a, sizeof a));
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->stop();
  adv->setAdvertisementData(d);
  adv->start();
  // Not millis() | 1 to make sure it isn't 0: that is up to 1 ms ahead, and loop(), on the other
  // core, then saw millis() - wakeStart wrap around to "over 30 s" and stopped the advertisement.
  wakeStart = millis();  // never 0: we're past boot
  dump("wake-up advertisement", a, sizeof a);
}

bool waking() { return wakeStart != 0; }

void sendRaw(uint8_t type, uint8_t set, uint8_t id, const uint8_t *p, size_t n) {
  if (ready() && n <= 46) send(type, set, id, p, n, seq++);  // 46 = room in send()'s frame buffer
}

}  // namespace dji
