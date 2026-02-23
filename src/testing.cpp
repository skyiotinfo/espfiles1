#include <ESP8266WiFi.h>
extern "C" {
  #include <espnow.h>
}

/************ CONFIG ************/
#define NODE_ID 2       // <<< CHANGE 1–18
#define SEND_INTERVAL 5000   // ms

/************ EXPECTED ************/
#define EXPECTED_ROTATION 20
#define EXPECTED_TILT      5

/************ ERROR FLAGS ************/
#define ROTATION_ERROR  0x01
#define TILT_ERROR      0x02
#define DOOR_ERROR      0x04

/************ TEST MODE ************/
#define TEST_MODE 1   // <<< SET TO 0 FOR REAL SENSORS

/************ MAIN BOARD MAC ************/
uint8_t mainBoardMAC[] = { 0x8C, 0x4F, 0x00, 0xE1, 0xE0, 0x75 };

/************ DATA PACKET ************/
typedef struct __attribute__((packed)) {
  uint8_t  nodeID;
  uint16_t rotationCount;
  uint16_t tiltCount;
  uint8_t  doorStatus;
  uint8_t  flags;
  uint32_t uptime;
} SensorPacket;

SensorPacket packet;
unsigned long lastSend = 0;

/************ SEND CALLBACK ************/
void onSend(uint8_t *mac, uint8_t status) {
  Serial.print("ESP-NOW Send: ");
  Serial.println(status == 0 ? "SUCCESS" : "FAIL");
}

/************ SETUP ************/
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n=== SENSOR NODE (TEST MODE) ===");
  Serial.print("Node ID: ");
  Serial.println(NODE_ID);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT FAILED");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSend);
  esp_now_add_peer(mainBoardMAC, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  Serial.println("ESP-NOW READY");
}

/************ SEND DATA ************/
void sendData() {

#if TEST_MODE
  packet.rotationCount = 18;   // ❌ ERROR (Expected 20)
  packet.tiltCount     = 5;    // ✅ OK
  packet.doorStatus    = 1;    // ❌ DOOR OPEN
#else
  packet.rotationCount = 0;
  packet.tiltCount     = 0;
  packet.doorStatus    = 0;
#endif

  packet.nodeID = NODE_ID;
  packet.uptime = millis();

  packet.flags = 0;
  if (packet.rotationCount != EXPECTED_ROTATION) packet.flags |= ROTATION_ERROR;
  if (packet.tiltCount != EXPECTED_TILT)         packet.flags |= TILT_ERROR;
  if (packet.doorStatus)                         packet.flags |= DOOR_ERROR;

  esp_now_send(mainBoardMAC, (uint8_t*)&packet, sizeof(packet));

  /******** SERIAL DEBUG ********/
  Serial.println("\n---- DATA SENT ----");
  Serial.print("Node ID: ");   Serial.println(packet.nodeID);
  Serial.print("Rotation: ");  Serial.println(packet.rotationCount);
  Serial.print("Tilt: ");      Serial.println(packet.tiltCount);
  Serial.print("Door: ");      Serial.println(packet.doorStatus ? "OPEN" : "CLOSED");
  Serial.print("Flags: 0b");   Serial.println(packet.flags, BIN);
  Serial.print("Uptime: ");    Serial.println(packet.uptime);

  if (packet.flags == 0) Serial.println("STATUS: NORMAL");
  if (packet.flags & ROTATION_ERROR) Serial.println("ERROR: ROTATION");
  if (packet.flags & TILT_ERROR)     Serial.println("ERROR: TILT");
  if (packet.flags & DOOR_ERROR)     Serial.println("ERROR: DOOR");

  Serial.println("-------------------");
}

/************ LOOP ************/
void loop() {
  if (millis() - lastSend > SEND_INTERVAL) {
    sendData();
    lastSend = millis();
  }
}
