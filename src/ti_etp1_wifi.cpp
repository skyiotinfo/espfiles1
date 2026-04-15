#include <Arduino.h>
#include <EEPROM.h>
#include <TM1637Display.h>
#include <ESP8266WiFi.h>
#include <espnow.h>

extern "C" {
  #include "user_interface.h"
}

#define M1_PIN    D5
#define M2_PIN    D6
#define B1_PIN    D7
#define B2_PIN    D8
#define BUTTON_PIN D3
#define CLK       D1
#define DIO       D2

TM1637Display display(CLK, DIO);

uint8_t board4Mac[] = {0x3C, 0x61, 0x05, 0xDC, 0x6A, 0x29};

uint8_t lastSentStatus = 255;
uint8_t pendingStatus = 0;
bool statusPending = false;
unsigned long statusSendTime = 0;
const unsigned long RETRY_INTERVAL = 300;

const int addrDuration = 0;
unsigned long runTime_ms;
unsigned long motorStartTime = 0;
bool runningFirstPair = true;
int motorDuration_min = 10;
bool systemBooting = true;

void sendStatus() {
  uint8_t status = 0;
  if (digitalRead(M1_PIN) && digitalRead(B1_PIN)) status = 1;
  else if (digitalRead(M2_PIN) && digitalRead(B2_PIN)) status = 2;

  if (status == lastSentStatus && !statusPending) return;

  pendingStatus = status;
  statusPending = true;
  statusSendTime = 0;
  Serial.print("📤 Queue status to Board4: ");
  Serial.println(status);
}

void trySendPendingStatus() {
  if (!statusPending) return;
  unsigned long now = millis();
  if (now - statusSendTime >= RETRY_INTERVAL) {
    statusSendTime = now;
    uint8_t data[2] = {2, pendingStatus};
    int result = esp_now_send(board4Mac, data, sizeof(data));
    Serial.print("📤 Sending status (");
    Serial.print(pendingStatus);
    Serial.print(") → ");
    Serial.println(result == 0 ? "OK" : "ERROR");
  }
}

void startM1B1() {
  Serial.println("▶ Start M1+B1");
  digitalWrite(M1_PIN, HIGH);
  digitalWrite(B1_PIN, HIGH);
  digitalWrite(M2_PIN, LOW);
  digitalWrite(B2_PIN, LOW);
  sendStatus();
}

void startM2B2() {
  Serial.println("▶ Start M2+B2");
  digitalWrite(M2_PIN, HIGH);
  digitalWrite(B2_PIN, HIGH);
  digitalWrite(M1_PIN, LOW);
  digitalWrite(B1_PIN, LOW);
  sendStatus();
}

void stopAll() {
  Serial.println("⏹ Stop all");
  digitalWrite(M1_PIN, LOW);
  digitalWrite(M2_PIN, LOW);
  digitalWrite(B1_PIN, LOW);
  digitalWrite(B2_PIN, LOW);
  sendStatus();
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len == 1 && data[0] == 0xAA) {
    Serial.println("✅ ACK from Board4");
    lastSentStatus = pendingStatus;
    statusPending = false;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  EEPROM.begin(512);
  pinMode(M1_PIN, OUTPUT);
  pinMode(M2_PIN, OUTPUT);
  pinMode(B1_PIN, OUTPUT);
  pinMode(B2_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  display.setBrightness(0x0f);

  motorDuration_min = EEPROM.read(addrDuration);
  if (motorDuration_min < 1 || motorDuration_min > 240) motorDuration_min = 10;
  display.showNumberDec(motorDuration_min);
  runTime_ms = motorDuration_min * 60000UL;

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print("Board2 MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(board4Mac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  startM1B1();
  motorStartTime = millis();
  systemBooting = false;
  Serial.println("✅ Board2 ready");
}

void loop() {
  trySendPendingStatus();

  if (digitalRead(BUTTON_PIN) == LOW && !systemBooting) {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW) {
      motorDuration_min++;
      if (motorDuration_min > 240) motorDuration_min = 1;
      display.showNumberDec(motorDuration_min);
      EEPROM.write(addrDuration, motorDuration_min);
      EEPROM.commit();
      runTime_ms = motorDuration_min * 60000UL;
      Serial.print("Duration set to ");
      Serial.print(motorDuration_min);
      Serial.println(" min");
      while (digitalRead(BUTTON_PIN) == LOW) delay(50);
    }
  }

  unsigned long elapsed = millis() - motorStartTime;
  if (elapsed >= runTime_ms) {
    stopAll();
    delay(1000);
    runningFirstPair = !runningFirstPair;
    if (runningFirstPair) startM1B1();
    else startM2B2();
    motorStartTime = millis();
  }

  delay(50);
}