#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" { 
  #include "user_interface.h" 
}

#define VALVE_PIN D8

//uint8_t valve1Mac[] = {0xC8, 0xC9, 0xA3, 0x39, 0xA4, 0xDE};
//uint8_t valve3Mac[] = {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0};
uint8_t valve3Mac[] = {0xC8, 0xC9, 0xA3, 0x39, 0xA4, 0xDE};
uint8_t valve1Mac[] = {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0};
bool     valveOn       = false;
uint16_t lastFwdSignal = 9999;

bool          waitingV3Ack = false;
unsigned long v3AckTimeout = 0;

const unsigned long ACK_TIMEOUT   = 3000UL;
const uint8_t       MAX_FWD_RETRY = 5;
const unsigned long COMMAND_TIMEOUT = 40000UL; // 40 seconds
unsigned long lastCommandTime = 0;
uint8_t  fwdRetryCount = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// V2 uses the tens digit (signal / 10) % 10
bool myBitOn(uint16_t signal) {
  return ((signal / 10) % 10) != 0;
}

// Strip V2's digit before forwarding downstream
uint16_t forwardSignal(uint16_t signal) {
  return signal % 10;
}

// Combine V2 state with V3 ack into a full ACK for V1
uint16_t buildAck(uint16_t v3ack) {
  return (valveOn ? 20 : 0) + v3ack;
}

// ─── Communication helpers ───────────────────────────────────────────────────

void forwardToValve3(uint16_t signal) {
  lastFwdSignal = signal;
  waitingV3Ack  = true;
  v3AckTimeout  = millis();
  fwdRetryCount = 0;
  esp_now_send(valve3Mac, (uint8_t*)&signal, sizeof(signal));
  Serial.printf("📤 Forward to V3: %03d\n", signal);
}

void sendAckToValve1(uint16_t ack) {
  esp_now_send(valve1Mac, (uint8_t*)&ack, sizeof(ack));
  Serial.printf("📤 ACK to V1    : %03d\n", ack);
}

// ─── ESP-NOW callbacks ───────────────────────────────────────────────────────

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 2) return;
  uint16_t msg = 0;
  memcpy(&msg, data, sizeof(uint16_t));

  bool fromV1 = memcmp(mac, valve1Mac, 6) == 0;
  bool fromV3 = memcmp(mac, valve3Mac, 6) == 0;

  if (fromV1) {
    lastCommandTime = millis();
    Serial.printf("📥 Command from V1: %03d\n", msg);

    bool shouldBeOn = myBitOn(msg);
    if (shouldBeOn && !valveOn) {
      digitalWrite(VALVE_PIN, HIGH);   // Active LOW
      valveOn = true;
      Serial.println("🚿 Valve 2 ON");
    } else if (!shouldBeOn && valveOn) {
      digitalWrite(VALVE_PIN, LOW);
      valveOn = false;
      Serial.println("🚿 Valve 2 OFF");
    }

    // ── KEY FIX: immediately ACK V2's own state to V1 ──
    // Don't wait for V3 — send V2 status right now so motor board
    // can start immediately even if V3 is offline or slow.
    uint16_t immediateAck = buildAck(0);  // V2 state + V3=0 (unknown yet)
    sendAckToValve1(immediateAck);
    Serial.printf("📤 Immediate ACK to V1: %03d (V2 state, V3 pending)\n", immediateAck);

    // Forward to V3 in parallel — its ACK will trigger a second update
    forwardToValve3(forwardSignal(msg));

  } else if (fromV3) {
    Serial.printf("📥 ACK from V3  : %03d\n", msg);
    waitingV3Ack  = false;
    fwdRetryCount = 0;

    // Send updated ACK now that V3 state is known
    uint16_t fullAck = buildAck(msg);

    // Only send updated ACK if V3 changed something (avoids duplicate 020+000 spam)
    if (msg != 0) {
      sendAckToValve1(fullAck);
      Serial.printf("📤 Updated ACK to V1: %03d (V2+V3 state)\n", fullAck);
    } else {
      Serial.printf("ℹ️  V3 confirmed OFF — no ACK update needed (already sent %03d)\n",
                    buildAck(0));
    }
  }
}

void onSent(uint8_t *mac, uint8_t status) {}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n🌱 Valve 2 Board booting..."));

  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW);  // HIGH = OFF (active LOW)

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print(F("Valve 2 MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println(F("❌ ESP-NOW init failed"));
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(valve1Mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  esp_now_add_peer(valve3Mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

  Serial.println(F("✅ Valve 2 ready"));
}

// ─── Loop ────────────────────────────────────────────────────────────────────

void loop() {
if (valveOn &&
    lastCommandTime > 0 &&
    millis() - lastCommandTime > COMMAND_TIMEOUT) {

    Serial.println(F("⚠️ Communication lost"));
    Serial.println(F("🚿 Valve 2 OFF"));

    digitalWrite(VALVE_PIN, LOW);
    valveOn = false;

    waitingV3Ack = false;
    fwdRetryCount = 0;

    uint16_t ack = 0;
    sendAckToValve1(ack);
}

  if (waitingV3Ack && millis() - v3AckTimeout > ACK_TIMEOUT) {

    if (fwdRetryCount >= MAX_FWD_RETRY) {
      // V3 is offline — V2 already sent its own state immediately on receive,
      // so no additional ACK needed here. Just log and clear.
      Serial.println(F("❌ No ACK from V3 after 5 attempts — V3 treated as offline"));
      Serial.println(F("ℹ️  V2 already reported its state — motor can run on V2 alone"));
      waitingV3Ack = false;

    } else {
      fwdRetryCount++;
      v3AckTimeout = millis();
      esp_now_send(valve3Mac, (uint8_t*)&lastFwdSignal, sizeof(lastFwdSignal));
      Serial.printf("🔄 Retry fwd to V3: %03d (attempt %d/%d)\n",
                    lastFwdSignal, fwdRetryCount, MAX_FWD_RETRY);
    }
  }
  delay(10);
}
