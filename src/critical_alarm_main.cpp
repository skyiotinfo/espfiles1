#include <ESP8266WiFi.h>
#include <espnow.h>


#define TOTAL_NODES 18
#define ALARM_PIN   5
#define ALARM_TIME 5000   // Alarm ON for 5 seconds

#define ROTATION_ERROR  0x01
#define TILT_ERROR      0x02
#define DOOR_ERROR      0x04

typedef struct __attribute__((packed)) {
  uint8_t  nodeID;
  uint16_t rotationCount;
  uint16_t tiltCount;
  uint8_t  doorStatus;
  uint8_t  flags;
  uint32_t uptime;
} SensorPacket;

unsigned long alarmStart = 0;
bool alarmActive = false;

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {

  if (len != sizeof(SensorPacket)) return;

  SensorPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));

  Serial.println("\n========== ALERT RECEIVED ==========");
  Serial.print("Node ID: ");
  Serial.println(pkt.nodeID);

  if (pkt.flags & ROTATION_ERROR)
    Serial.println("ERROR: Rotation < 2");

  if (pkt.flags & TILT_ERROR)
    Serial.println("ERROR: Tilt < 2");

  if (pkt.flags & DOOR_ERROR)
    Serial.println("ERROR: Door Open");

  Serial.println("====================================");

  digitalWrite(ALARM_PIN, HIGH);
  alarmActive = true;
  alarmStart = millis();
}

void setup() {

  Serial.begin(115200);

  pinMode(ALARM_PIN, OUTPUT);
  digitalWrite(ALARM_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW INIT FAILED");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onReceive);

}

void loop() {

  if (alarmActive) {
    if (millis() - alarmStart >= ALARM_TIME) {
      digitalWrite(ALARM_PIN, LOW);
      alarmActive = false;
      Serial.println("Alarm Reset");
    }
  }
}
