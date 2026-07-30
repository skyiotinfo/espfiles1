

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <ArduinoJson.h>
extern "C" {
  #include "user_interface.h"
}

uint8_t motorMac[6] = {0xFC, 0xF5, 0xC4, 0xBE, 0x5D, 0xD9};

const uint8_t NUM_NODES = 4;
const unsigned long STALE_WARN_MS = 20000UL;

struct StatusPacket {
  uint8_t  pumpOn;
  uint8_t  doorOpen[3];
  uint8_t  interlockOk;
  uint8_t  nodeOpen[5];
  uint8_t  nodeEverSeen[5];
  uint16_t nodeLastSeenSec[5];
};

volatile StatusPacket rxPacket;
volatile bool rxAvailable = false;
volatile uint8_t rxLen = 0;

bool everReceivedPacket = false;
unsigned long lastPacketAt = 0;
unsigned long packetCount = 0;

#define LINK_BAUD   115200   
#define DEBUG_BAUD  115200
#define DEVICE_ID   "monitor-board-1"

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(StatusPacket)) return;
  memcpy((void *)&rxPacket, data, sizeof(StatusPacket));
  rxLen = len;
  rxAvailable = true;
}

void onSent(uint8_t *mac, uint8_t status) {}

void sendToRelay(const StatusPacket &pkt) {
  StaticJsonDocument<768> doc;
  doc["device_id"] = DEVICE_ID;
  doc["pump"] = pkt.pumpOn ? 1 : 0;

  doc["door1"] = pkt.doorOpen[0] ? 1 : 0;
  doc["door2"] = pkt.doorOpen[1] ? 1 : 0;
  doc["door3"] = pkt.doorOpen[2] ? 1 : 0;

  doc["interlock"] = pkt.interlockOk ? 1 : 0;
  doc["pkt"] = packetCount;

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    String openKey = "node" + String(i) + "_open";
    String seenKey = "node" + String(i) + "_seen";
    String lastKey = "node" + String(i) + "_last";
    doc[openKey] = pkt.nodeOpen[i] ? 1 : 0;
    doc[seenKey] = pkt.nodeEverSeen[i] ? 1 : 0;
    doc[lastKey] = pkt.nodeLastSeenSec[i];
  }

  serializeJson(doc, Serial); 
  Serial.println();           
}

void debugPrintPacket(const StatusPacket &pkt) {
  Serial1.println(F("\n===== MOTOR BOARD STATUS ====="));
  Serial1.printf("Pump  : %s\n", pkt.pumpOn ? "ON" : "OFF");
  Serial1.printf("Doors : D1=%s D2=%s D3=%s -> interlock %s\n",
                  pkt.doorOpen[0] ? "OPEN" : "closed",
                  pkt.doorOpen[1] ? "OPEN" : "closed",
                  pkt.doorOpen[2] ? "OPEN" : "closed",
                  pkt.interlockOk ? "OK" : "BLOCKED");
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (pkt.nodeEverSeen[i]) {
      Serial1.printf("Node %d : %s (last seen %us ago)\n",
                      i, pkt.nodeOpen[i] ? "OPEN" : "closed", pkt.nodeLastSeenSec[i]);
    } else {
      Serial1.printf("Node %d : %s (never seen)\n", i, pkt.nodeOpen[i] ? "OPEN" : "closed");
    }
  }
  Serial1.printf("Packet #%lu -> forwarded to relay board\n", packetCount);
  Serial1.println(F("===============================\n"));
}

void handleQueued() {
  if (!rxAvailable) return;

  noInterrupts();
  StatusPacket local;
  memcpy(&local, (const void *)&rxPacket, sizeof(StatusPacket));
  rxAvailable = false;
  interrupts();

  everReceivedPacket = true;
  lastPacketAt = millis();
  packetCount++;

  sendToRelay(local);
  debugPrintPacket(local);
}

void checkStale() {
  static unsigned long lastWarnAt = 0;
  if (!everReceivedPacket) return;

  if (millis() - lastPacketAt > STALE_WARN_MS) {
    if (millis() - lastWarnAt > STALE_WARN_MS) {
      Serial1.printf("WARNING: no packet from motor board in %lus - is it powered / in range?\n",
                      (millis() - lastPacketAt) / 1000);
      lastWarnAt = millis();
    }
  }
}

void setup() {
  Serial.begin(LINK_BAUD);    
  Serial1.begin(DEBUG_BAUD); 

  Serial1.println(F("\nMonitor Board (ESP07) booting..."));

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);

  Serial1.print(F("This monitor's MAC: "));
  Serial1.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial1.println(F("ESP-NOW init failed"));
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(motorMac, ESP_NOW_ROLE_CONTROLLER, 1, NULL, 0);

  Serial1.println(F("Monitor Board ready - waiting for status packets...\n"));
}

void loop() {
  handleQueued();
  checkStale();
  delay(50);
}
