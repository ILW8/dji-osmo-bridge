// Step 1 test: shows up on the ELEMNT as a cadence sensor that never sends
// cadence, only a battery level. Type 0-100 + Enter in the Serial Monitor
// (115200 baud) to change the reported battery and watch the ELEMNT follow.
#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>

BLECharacteristic *battery;
uint8_t level = 42;

class Callbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { Serial.println("ELEMNT connected"); }
  void onDisconnect(BLEServer *) override {
    Serial.println("disconnected, advertising again");
    BLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);
  BLEDevice::init("Osmo Battery");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new Callbacks());

  // Cycling Speed and Cadence service, only so the ELEMNT will pair.
  // ponytail: the measurement characteristic never notifies, so no cadence is ever recorded.
  BLEService *csc = server->createService(BLEUUID((uint16_t)0x1816));
  csc->createCharacteristic(BLEUUID((uint16_t)0x2A5B), BLECharacteristic::PROPERTY_NOTIFY)
     ->addDescriptor(new BLE2902());
  uint8_t feature[] = {0x02, 0x00};  // "crank revolution data supported" = cadence sensor
  csc->createCharacteristic(BLEUUID((uint16_t)0x2A5C), BLECharacteristic::PROPERTY_READ)
     ->setValue(feature, sizeof feature);
  csc->start();

  // Standard Battery Service: this is the part the ELEMNT should display.
  BLEService *bas = server->createService(BLEUUID((uint16_t)0x180F));
  battery = bas->createCharacteristic(BLEUUID((uint16_t)0x2A19),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  battery->addDescriptor(new BLE2902());
  battery->setValue(&level, 1);
  bas->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLEUUID((uint16_t)0x1816));
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("advertising as 'Osmo Battery'");
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
  battery->notify();
  Serial.printf("battery now %d%%\n", level);
}
