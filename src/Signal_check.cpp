#include <SPI.h>
#include <LoRa.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define LORA_SS   15
#define LORA_RST  16
#define LORA_DIO0 2
const long LORA_FREQ = 433920000;

#define SERVICE_UUID     "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_NOTIFY_UUID "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_READ_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

BLECharacteristic* pNotifyChar;
BLECharacteristic* pReadChar;
bool deviceConnected = false;

String lastLoRaPayload = "";
int lastLoRaRssi = 0;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE connected");
  }
  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE disconnected");
    pServer->getAdvertising()->start();
  }
};

void setup() {
  Serial.begin(115200);
  delay(100);

  LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);
  LoRa.setSyncWord(0xA2);
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setTxPower(20);

  Serial.println("Starting LoRa...");
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println("LoRa init failed. Check wiring.");
    while (true) {
      delay(1000);
    }
  }
  Serial.println("LoRa OK");

  BLEDevice::init("ESP32_LoRa");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pNotifyChar = pService->createCharacteristic(
    CHAR_NOTIFY_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pNotifyChar->addDescriptor(new BLE2902());

  pReadChar = pService->createCharacteristic(
    CHAR_READ_UUID,
    BLECharacteristic::PROPERTY_READ
  );

  pService->start();
  pServer->getAdvertising()->start();
  Serial.println("BLE advertising started.");
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize) {
    String payload = "";
    while (LoRa.available()) {
      char c = (char)LoRa.read();
      payload += c;
    }
    payload.trim();

    int rssi = LoRa.packetRssi();

    lastLoRaPayload = payload;
    lastLoRaRssi = rssi;

    String notifyMsg = payload + ";RSSI:" + String(rssi);

    Serial.print("LoRa packet: "); Serial.print(payload);
    Serial.print("  RSSI: "); Serial.println(rssi);

    pReadChar->setValue(notifyMsg.c_str());

    if (deviceConnected) {
      pNotifyChar->setValue(notifyMsg.c_str());
      pNotifyChar->notify();
      delay(10);
    }
  }
  delay(5);
}
