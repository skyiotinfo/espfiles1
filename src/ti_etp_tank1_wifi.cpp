
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
extern "C" { 
  #include "user_interface.h" 
}

#define MOTOR_PIN D8
#define LED_PIN   D4
#define UT_SENSOR D2

const unsigned long MAX_RUNTIME = 60UL * 60UL * 1000UL; 

uint8_t senderMac[] = {0x68,0xC6,0x3A,0xF7,0x36,0x63};

#define EEPROM_ADDR 0

unsigned long motorStartTime = 0;
bool motorRunning = false;
uint8_t lastCommand = 255;
unsigned long lastMsgTime = 0;
const unsigned long MSG_TIMEOUT = 30000;   

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len);

void sendAck() {
  uint8_t ack = 2;
  esp_now_send(senderMac, &ack, sizeof(ack));
  Serial.println("ACK Sent");
}

void turnMotorON() {
  Serial.println("Motor ON");
  digitalWrite(MOTOR_PIN, HIGH);
  digitalWrite(LED_PIN, HIGH);
  motorStartTime = millis();
  motorRunning = true;
  EEPROM.write(EEPROM_ADDR, 1);
  EEPROM.commit();
}

void turnMotorOFF() {
  Serial.println("Motor OFF");
  digitalWrite(MOTOR_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  motorRunning = false;
  EEPROM.write(EEPROM_ADDR, 0);
  EEPROM.commit();
}

void setup() {
  Serial.begin(115200);
  pinMode(MOTOR_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(UT_SENSOR, INPUT_PULLUP);
  digitalWrite(MOTOR_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
  EEPROM.begin(10);
  byte savedState = EEPROM.read(EEPROM_ADDR);
  if (savedState == 1) {
    Serial.println("Restoring Motor ON");
    digitalWrite(MOTOR_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH);
    motorRunning = true;
    motorStartTime = millis();
  }
  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print("Motor Board MAC: ");
  Serial.println(WiFi.macAddress());
  if (esp_now_init() != 0) {
    Serial.println("ESP NOW FAIL");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(senderMac, ESP_NOW_ROLE_CONTROLLER, 1, NULL, 0);
  lastMsgTime = millis();
  Serial.println("Receiver Ready");
}

void loop() {
  bool tankEmpty = digitalRead(UT_SENSOR) == LOW;
  if (motorRunning && tankEmpty) {
    Serial.println("UT EMPTY → Motor OFF");
    turnMotorOFF();
    sendAck();
  }
  if (motorRunning && millis() - motorStartTime > MAX_RUNTIME) {
    Serial.println("Timeout → Motor OFF");
    turnMotorOFF();
    sendAck();
  }
  if (motorRunning && (millis() - lastMsgTime > MSG_TIMEOUT)) {
    Serial.println("⚠️ No message for 15s → Motor OFF");
    turnMotorOFF();
  }
  delay(50);
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  lastMsgTime = millis();   
  uint8_t value = data[0];
  Serial.print("Received: ");
  Serial.println(value);
  if (value == lastCommand) {
    Serial.println("Duplicate ignored");
    sendAck();
    return;
  }
  lastCommand = value;
  bool tankEmpty = digitalRead(UT_SENSOR) == LOW;
  if (value == 1) {
    if (tankEmpty) {
      Serial.println("UT EMPTY → Start Not Allowed");
      sendAck();
      return;
    }
    if (!motorRunning) turnMotorON();
    sendAck();
  } else if (value == 0) {
    if (motorRunning) turnMotorOFF();
    sendAck();
  }
}