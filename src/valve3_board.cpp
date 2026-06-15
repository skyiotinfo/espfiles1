#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" { 
  #include "user_interface.h" 
}

#define VALVE_PIN D8

uint8_t valve2Mac[] = {0x48, 0x55, 0x19, 0xEC, 0xAA, 0xCF};

bool valveOn = false;
const unsigned long COMMAND_TIMEOUT = 40000UL; // 40 sec
unsigned long lastCommandTime = 0;

bool myBitOn(uint16_t signal) {
  return (signal % 10) != 0;
}

void sendAckToValve2(uint16_t ack) {
  esp_now_send(valve2Mac, (uint8_t*)&ack, sizeof(ack));
  Serial.printf("📤 ACK to V2: %03d\n", ack);
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 2) return;
    lastCommandTime = millis();
  uint16_t msg = 0;
  memcpy(&msg, data, sizeof(uint16_t));

  Serial.printf("📥 Command from V2: %03d\n", msg);

  bool shouldBeOn = myBitOn(msg);
  if (shouldBeOn && !valveOn) {
    digitalWrite(VALVE_PIN, HIGH);
    valveOn = true;
    Serial.println("🚿 Valve 3 ON");
  } else if (!shouldBeOn && valveOn) {
    digitalWrite(VALVE_PIN, LOW);
    valveOn = false;
    Serial.println("🚿 Valve 3 OFF");
  }

  uint16_t ack = valveOn ? 2 : 0;
  sendAckToValve2(ack);
}

void onSent(uint8_t *mac, uint8_t status) {}

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n🌱 Valve 3 Board booting..."));

  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW);

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print(F("Valve 3 MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) { 
    Serial.println(F("❌ ESP-NOW init failed")); 
    return; 
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(valve2Mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

  Serial.println(F("✅ Valve 3 ready"));
}

void loop() {

  if (valveOn &&
      lastCommandTime > 0 &&
      millis() - lastCommandTime > COMMAND_TIMEOUT) {

    Serial.println(F("⚠️ Communication lost"));
    Serial.println(F("🚿 Valve 3 OFF"));

    digitalWrite(VALVE_PIN, LOW);   // OFF for your wiring
    valveOn = false;

    uint16_t ack = 0;
    sendAckToValve2(ack);
  }

  delay(10);
}