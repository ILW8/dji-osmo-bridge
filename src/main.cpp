// Step 1: shows up on the ELEMNT as a Wahoo radar (by name) that only reports a battery level.
// Type 0-100 + Enter in the Serial Monitor (115200 baud) to change the reported battery and
// watch the ELEMNT's sensor ring follow. See HANDOFF.md for why the name matters.
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

BLEServer *server;
BLECharacteristic *battery;
BLECharacteristic *measurement;
BLE2902 *batteryCccd;
uint8_t level = 42;
// Must match ^TRACKR RADAR( [0-9A-F]{4,5})?$ exactly. No hex suffix: the ELEMNT parses it as an
// ANT+ device ID and switches the sensor to ANT+ radar + light, dropping the BLE link.
const char *kName = "TRACKR RADAR";

// Debug: log every GATT read/write the ELEMNT makes, by attribute handle.
// Match handles against the map printed at boot.
void logGatt(esp_gatts_cb_event_t event, esp_gatt_if_t, esp_ble_gatts_cb_param_t *p) {
  if (event == ESP_GATTS_READ_EVT) {
    Serial.printf("GATT read  handle %u\n", p->read.handle);
  } else if (event == ESP_GATTS_WRITE_EVT) {
    Serial.printf("GATT write handle %u:", p->write.handle);
    for (int i = 0; i < p->write.len; i++) Serial.printf(" %02x", p->write.value[i]);
    Serial.println();
  }
}

class BatteryRead : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *c) override {
    Serial.printf("  answered battery read with %u%%\n", c->getData()[0]);
  }
};

class Callbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { Serial.println("ELEMNT connected"); }
  void onDisconnect(BLEServer *) override {
    Serial.println("disconnected, advertising again");
    BLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);
  BLEDevice::setCustomGattsHandler(logGatt);
  // ELEMNT firmware ignores BLE battery for cadence-type sensors. A name matching its Wahoo radar
  // pattern ^TRACKR RADAR( [0-9A-F]{4,5})?$ overrides the product type, and radar battery is honored.
  BLEDevice::init(kName);
  server = BLEDevice::createServer();
  server->setCallbacks(new Callbacks());

  // Cycling Speed and Cadence service, only so the ELEMNT will pair.
  // No measurement is ever sent, so there is never a cadence value to record.
  BLEService *csc = server->createService(BLEUUID((uint16_t)0x1816));
  measurement =
      csc->createCharacteristic(BLEUUID((uint16_t)0x2A5B), BLECharacteristic::PROPERTY_NOTIFY);
  BLE2902 *measurementCccd = new BLE2902();
  measurement->addDescriptor(measurementCccd);
  uint8_t feature[] = {0x02, 0x00};  // "crank revolution data supported" = cadence sensor
  BLECharacteristic *featureChr =
      csc->createCharacteristic(BLEUUID((uint16_t)0x2A5C), BLECharacteristic::PROPERTY_READ);
  featureChr->setValue(feature, sizeof feature);
  csc->start();

  // Standard Battery Service: this is the part the ELEMNT should display.
  BLEService *bas = server->createService(BLEUUID((uint16_t)0x180F));
  battery = bas->createCharacteristic(BLEUUID((uint16_t)0x2A19),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  batteryCccd = new BLE2902();
  battery->addDescriptor(batteryCccd);
  battery->setValue(&level, 1);
  battery->setCallbacks(new BatteryRead());
  bas->start();

  // Device Information Service: real sensors identify themselves; test whether the ELEMNT needs it.
  BLEService *dis = server->createService(BLEUUID((uint16_t)0x180A));
  dis->createCharacteristic(BLEUUID((uint16_t)0x2A29), BLECharacteristic::PROPERTY_READ)
     ->setValue("DIY");                  // manufacturer name
  dis->createCharacteristic(BLEUUID((uint16_t)0x2A24), BLECharacteristic::PROPERTY_READ)
     ->setValue("Osmo Battery Bridge");  // model number
  dis->createCharacteristic(BLEUUID((uint16_t)0x2A26), BLECharacteristic::PROPERTY_READ)
     ->setValue("0.1");                  // firmware revision
  dis->start();

  Serial.printf("handles: cadence measurement %u (subscribe %u), cadence feature %u, "
                "battery level %u (subscribe %u)\n",
                measurement->getHandle(), measurementCccd->getHandle(), featureChr->getHandle(),
                battery->getHandle(), batteryCccd->getHandle());

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLEUUID((uint16_t)0x1816));
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("advertising as '%s'\n", kName);
}

void loop() {
  if (!Serial.available()) return;
  String s = Serial.readStringUntil('\n');
  s.trim();
  if (s.length() == 0 || !isDigit(s[0])) return;
  int v = s.toInt();
  if (v > 100) return;
  level = v;
  battery->setValue(&level, 1);
  // The ELEMNT never subscribes (no write to the battery CCCD) and only re-reads now and then,
  // but it still accepts a notification. Faking our side of the subscription lets notify() send.
  batteryCccd->setNotifications(true);
  battery->notify();
  Serial.printf("battery now %d%% (notified)\n", level);
}
