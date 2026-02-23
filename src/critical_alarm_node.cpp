#include <ESP8266WiFi.h>
#include <espnow.h>


#define NODE_ID 2              

#define HALL_ROT_PIN   12
#define HALL_TILT_PIN  13
#define DOOR_PIN       14

#define CHECK_INTERVAL 20000   // 20 seconds
#define MIN_ROTATION   2
#define MIN_TILT       2

#define ROTATION_ERROR  0x01
#define TILT_ERROR      0x02
#define DOOR_ERROR      0x04

uint8_t mainBoardMAC[] = { 0x48, 0x55, 0x19, 0xEC, 0xAA, 0xCF }; 

typedef struct __attribute__((packed)) {
  uint8_t  nodeID;
  uint16_t rotationCount;
  uint16_t tiltCount;
  uint8_t  doorStatus;
  uint8_t  flags;
  uint32_t uptime;
} SensorPacket;

SensorPacket packet;

volatile uint16_t rotationCount = 0;
volatile uint16_t tiltCount = 0;

unsigned long lastCheck = 0;

void IRAM_ATTR rotationISR() {
  rotationCount++;
}

void IRAM_ATTR tiltISR() {
  tiltCount++;
}

void setup() {
  Serial.begin(115200);

  pinMode(HALL_ROT_PIN, INPUT_PULLUP);
  pinMode(HALL_TILT_PIN, INPUT_PULLUP);
  pinMode(DOOR_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(HALL_ROT_PIN), rotationISR, RISING);
  attachInterrupt(digitalPinToInterrupt(HALL_TILT_PIN), tiltISR, RISING);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT FAILED");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(mainBoardMAC, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  Serial.println("NODE READY");
}

void checkAndSend() {

  packet.nodeID = NODE_ID;
  packet.rotationCount = rotationCount;
  packet.tiltCount = tiltCount;
  packet.doorStatus = (digitalRead(DOOR_PIN) == HIGH) ? 1 : 0;
  packet.uptime = millis();
  packet.flags = 0;

  if (rotationCount < MIN_ROTATION)
    packet.flags |= ROTATION_ERROR;

  if (tiltCount < MIN_TILT)
    packet.flags |= TILT_ERROR;

  if (packet.doorStatus == 1)
    packet.flags |= DOOR_ERROR;

  if (packet.flags != 0) {

    esp_now_send(mainBoardMAC, (uint8_t*)&packet, sizeof(packet));

    Serial.println("\n*** ERROR SENT ***");
    Serial.print("Node: "); Serial.println(NODE_ID);

    if (packet.flags & ROTATION_ERROR) Serial.println("ERROR: Rotation < 2");
    if (packet.flags & TILT_ERROR)     Serial.println("ERROR: Tilt < 2");
    if (packet.flags & DOOR_ERROR)     Serial.println("ERROR: Door Open");

    Serial.println("------------------");
  }
  else {
    Serial.println("OK - No Error");
  }

  // Reset counters
  rotationCount = 0;
  tiltCount = 0;
}

void loop() {

  if (millis() - lastCheck >= CHECK_INTERVAL) {
    checkAndSend();
    lastCheck = millis();
  }
}
