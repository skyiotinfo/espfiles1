#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" { 
  #include "user_interface.h" 
}

#define VALVE_PIN D8

uint8_t motorMac[]  = {0x3C, 0x61, 0x05, 0xDC, 0x6A, 0x29};
uint8_t valve2Mac[] = {0xFC, 0xF5, 0xC4, 0xBE, 0xAA, 0xA8};
//fc:f5:c4:be:aa:a8
bool     valveOn       = false;
uint16_t lastFwdSignal = 9999;

bool          waitingV2Ack  = false;
unsigned long v2AckTimeout  = 0;

const unsigned long ACK_TIMEOUT   = 3000UL;
const uint8_t       MAX_FWD_RETRY = 5;

const unsigned long COMMAND_TIMEOUT = 30000UL; // 90 seconds

unsigned long lastCommandTime = 0;

uint8_t  fwdRetryCount = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// V1 uses the hundreds digit (signal / 100) % 10
bool myBitOn(uint16_t signal) {
  return ((signal / 100) % 10) != 0;
}

// Strip V1's digit before forwarding downstream
uint16_t forwardSignal(uint16_t signal) {
  return signal % 100;
}

// Combine V1 state with V2+V3 ack into a full ACK for the motor
uint16_t buildAck(uint16_t v2v3ack) {
  return (valveOn ? 200 : 0) + v2v3ack;
}

// ─── Communication helpers ───────────────────────────────────────────────────

void forwardToValve2(uint16_t signal) {
  lastFwdSignal  = signal;
  waitingV2Ack   = true;
  v2AckTimeout   = millis();
  fwdRetryCount  = 0;
  esp_now_send(valve2Mac, (uint8_t*)&signal, sizeof(signal));
  Serial.printf("📤 Forward to V2: %03d\n", signal);
}

void sendAckToMotor(uint16_t ack) {
  esp_now_send(motorMac, (uint8_t*)&ack, sizeof(ack));
  Serial.printf("📤 ACK to Motor : %03d\n", ack);
}

// ─── ESP-NOW callbacks ───────────────────────────────────────────────────────

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 2) return;
  uint16_t msg = 0;
  memcpy(&msg, data, sizeof(uint16_t));

  bool fromMotor = memcmp(mac, motorMac,  6) == 0;
  bool fromV2    = memcmp(mac, valve2Mac, 6) == 0;

  if (fromMotor) {
    lastCommandTime = millis();
    Serial.printf("📥 Command from Motor: %03d\n", msg);

    bool shouldBeOn = myBitOn(msg);
    if (shouldBeOn && !valveOn) {
      digitalWrite(VALVE_PIN, HIGH);   // Active LOW
      valveOn = true;
      Serial.println("🚿 Valve 1 ON");
    } else if (!shouldBeOn && valveOn) {
      digitalWrite(VALVE_PIN,   LOW);
      valveOn = false;
      Serial.println("🚿 Valve 1 OFF");
    }

  
    uint16_t immediateAck = buildAck(0);  
    sendAckToMotor(immediateAck);
    Serial.printf("📤 Immediate ACK to Motor: %03d (V1 state, V2/V3 pending)\n", immediateAck);

    forwardToValve2(forwardSignal(msg));

  } else if (fromV2) {
    Serial.printf("📥 ACK from V2  : %03d\n", msg);
    waitingV2Ack  = false;
    fwdRetryCount = 0;

    uint16_t fullAck = buildAck(msg);

    if (msg != 0) {
      sendAckToMotor(fullAck);
      Serial.printf("📤 Updated ACK to Motor: %03d (V1+V2+V3 state)\n", fullAck);
    } else {
      Serial.printf("ℹ️  V2+V3 confirmed OFF — no ACK update needed (already sent %03d)\n",
                    buildAck(0));
    }
  }
}

void onSent(uint8_t *mac, uint8_t status) {}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n🌱 Valve 1 Board booting..."));

  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW);  // HIGH = OFF (active LOW)

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print(F("Valve 1 MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println(F("❌ ESP-NOW init failed"));
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(motorMac,  ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  esp_now_add_peer(valve2Mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

  Serial.println(F("✅ Valve 1 ready"));
}

void loop() {

if (valveOn &&
    lastCommandTime > 0 &&
    millis() - lastCommandTime > COMMAND_TIMEOUT) {

    Serial.println(F("⚠️ Communication lost"));
    Serial.println(F("🚿 Valve 1 OFF"));

    digitalWrite(VALVE_PIN, LOW);   // OFF
    valveOn = false;

    waitingV2Ack = false;
    fwdRetryCount = 0;

    uint16_t ack = 0;
    sendAckToMotor(ack);
    lastCommandTime = 0;
}

  if (waitingV2Ack && millis() - v2AckTimeout > ACK_TIMEOUT) {

    if (fwdRetryCount >= MAX_FWD_RETRY) {

      Serial.println(F("❌ No ACK from V2 after 5 attempts — V2 & V3 treated as offline"));
      Serial.println(F("ℹ️  V1 already reported its state — motor can run on V1 alone"));
      waitingV2Ack = false;

    } else {

      fwdRetryCount++;
      v2AckTimeout = millis();

      esp_now_send(valve2Mac,
                   (uint8_t*)&lastFwdSignal,
                   sizeof(lastFwdSignal));

      Serial.printf("🔄 Retry fwd to V2: %03d (attempt %d/%d)\n",
                    lastFwdSignal,
                    fwdRetryCount,
                    MAX_FWD_RETRY);
    }
  }
//
  delay(10);
}