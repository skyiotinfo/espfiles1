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
#define BUTTON_PIN D9
#define CLK       D1
#define DIO       D2

TM1637Display display(CLK, DIO);

uint8_t board4Mac[] = {0xA4, 0xCF, 0x12, 0xED, 0xB2, 0x5F};

void sendStatus();

uint8_t lastAckedStatus = 255;
uint8_t currentStatus = 0;
uint8_t pendingStatus = 0;
bool statusPending = false;
unsigned long lastSendAttempt = 0;
const unsigned long RETRY_INTERVAL = 300;

unsigned long motorStartTime = 0;
unsigned long runTime_ms = 0;
int motorDuration_min = 10;
bool runningFirstPair = true;
bool systemBooting = true;

enum MotorState { RUNNING, STOPPING, SWITCHING };
MotorState state = RUNNING;
unsigned long stateChangeTime = 0;

bool lastButtonState = HIGH;
bool buttonConfirmed = false;
unsigned long lastDebounceTime = 0;
const unsigned long DEBOUNCE_DELAY = 50;

void setMotors(bool m1, bool b1, bool m2, bool b2) {
  digitalWrite(M1_PIN, m1 ? HIGH : LOW);
  digitalWrite(B1_PIN, b1 ? HIGH : LOW);
  digitalWrite(M2_PIN, m2 ? HIGH : LOW);
  digitalWrite(B2_PIN, b2 ? HIGH : LOW);
}

void startM1B1() {
  Serial.println("▶ Start M1+B1");
  setMotors(true, true, false, false);
  currentStatus = 1;
  sendStatus();          
}

void startM2B2() {
  Serial.println("▶ Start M2+B2");
  setMotors(false, false, true, true);
  currentStatus = 2;
  sendStatus();
}

void stopAll() {
  Serial.println("⏹ Stop all");
  setMotors(false, false, false, false);
  currentStatus = 0;
  sendStatus();
}

void sendStatus() {
  if (currentStatus == lastAckedStatus && !statusPending) return;

  pendingStatus = currentStatus;
  statusPending = true;
  lastSendAttempt = millis();
  Serial.print("📤 Queued status: ");
  Serial.println(pendingStatus);
}

void trySendPendingStatus() {
  if (!statusPending) return;

  if (millis() - lastSendAttempt >= RETRY_INTERVAL) {
    lastSendAttempt = millis();
    uint8_t data[2] = {2, pendingStatus};
    int result = esp_now_send(board4Mac, data, sizeof(data));
    if (result == 0) {
      Serial.print("📤 Sent status ");
      Serial.println(pendingStatus);
    } else {
      Serial.print("❌ Send failed, code ");
      Serial.println(result);
    }
  }
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len == 1 && data[0] == 0xAA) {
    Serial.println("✅ ACK received");
    if (statusPending && pendingStatus == currentStatus) {
      lastAckedStatus = pendingStatus;
      statusPending = false;
    } else {
      Serial.println("⚠️ Ignoring stale ACK");
    }
  }
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN) == LOW;

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonConfirmed) {
      buttonConfirmed = reading;
      if (buttonConfirmed && !systemBooting) {
        motorDuration_min++;
        if (motorDuration_min > 240) motorDuration_min = 1;
        display.showNumberDec(motorDuration_min, true);
        EEPROM.write(0, motorDuration_min);
        EEPROM.commit();
        runTime_ms = motorDuration_min * 60000UL;
        Serial.print("Duration set to ");
        Serial.print(motorDuration_min);
        Serial.println(" min");
      }
    }
  }
  lastButtonState = reading;
}

void updateMotorTimer() {
  switch (state) {
    case RUNNING:
      if (millis() - motorStartTime >= runTime_ms) {
        stopAll();
        state = STOPPING;
        stateChangeTime = millis();
      }
      break;

    case STOPPING:
      if (millis() - stateChangeTime >= 1000) {
        runningFirstPair = !runningFirstPair;
        if (runningFirstPair) startM1B1();
        else startM2B2();
        motorStartTime = millis();
        state = RUNNING;
      }
      break;

    case SWITCHING:
      break;
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
  stopAll();
  int saved = EEPROM.read(0);
  if (saved >= 1 && saved <= 240) motorDuration_min = saved;
  else motorDuration_min = 10;
  display.showNumberDec(motorDuration_min, true);
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
  ESP.wdtFeed();
  trySendPendingStatus();
  handleButton();
  updateMotorTimer();
  delay(10);
}