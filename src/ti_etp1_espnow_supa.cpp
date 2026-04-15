// ============================================================
// Gateway.ino – collects ESP‑NOW data and sends via Serial
// ============================================================

#include <Arduino.h>
#include <espnow.h>
#include <ESP8266WiFi.h>

extern "C" {
  #include "user_interface.h"
}

// ---------- Track last known states ----------
int lastTank1 = -1, lastTank2 = -1, lastM3 = -1;
int lastM1 = -1, lastB1 = -1, lastM2 = -1, lastB2 = -1;

// ---------- Send a motor state change over Serial ----------
void sendMotorState(const String &id, int state) {
  Serial.print(id);
  Serial.print(',');
  Serial.println(state);
  Serial.flush();  // ensure data is sent
}

// ---------- ESP‑NOW receive callback ----------
void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != 2) return;   // we expect [board, status]

  uint8_t board = data[0];
  uint8_t status = data[1];

  // Send ACK back to the sender (so they stop retrying)
  uint8_t ack = 0xAA;
  esp_now_send(mac, &ack, 1);

  if (board == 1) {
    // Board1: bits: tank1 (bit2), tank2 (bit1), M3 (bit0)
    int tank1 = (status >> 2) & 1;
    int tank2 = (status >> 1) & 1;
    int m3    = status & 1;

    if (tank1 != lastTank1) { lastTank1 = tank1; sendMotorState("tank1", tank1); }
    if (tank2 != lastTank2) { lastTank2 = tank2; sendMotorState("tank2", tank2); }
    if (m3    != lastM3)    { lastM3    = m3;    sendMotorState("M3",    m3); }
  }
  else if (board == 2) {
    // Board2: status 0 = all off, 1 = M1+B1 on, 2 = M2+B2 on
    int m1=0, b1=0, m2=0, b2=0;
    if (status == 1) { m1 = 1; b1 = 1; }
    else if (status == 2) { m2 = 1; b2 = 1; }

    if (m1 != lastM1) { lastM1 = m1; sendMotorState("M1", m1); }
    if (b1 != lastB1) { lastB1 = b1; sendMotorState("B1", b1); }
    if (m2 != lastM2) { lastM2 = m2; sendMotorState("M2", m2); }
    if (b2 != lastB2) { lastB2 = b2; sendMotorState("B2", b2); }
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(100);

  // Set WiFi to STA mode and fix channel (must match the other boards)
  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);

  Serial.print("Gateway MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onReceive);

  // Add a broadcast peer so we can send ACKs to any sender
  uint8_t broadcast[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_add_peer(broadcast, ESP_NOW_ROLE_COMBO, 1, NULL, 0);

  Serial.println("Gateway ready – forwarding ESP‑NOW data to Serial");
}

void loop() {
  // Nothing to do here – everything is event‑driven
  delay(10);
}