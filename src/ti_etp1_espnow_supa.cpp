#include <Arduino.h>
#include <espnow.h>
#include <ESP8266WiFi.h>

extern "C" {
  #include "user_interface.h"
}

int motorStates[8] = {0,0,0,0,0,0,0,0};
int lastSentStates[8] = {-1,-1,-1,-1,-1,-1,-1,-1};

void sendCompactState() {
  String data = "";
  for (int i = 0; i < 8; i++) {
    data += String(motorStates[i]);
  }
  char motor_st = 'S';
  for (int i = 0; i < 8; i++) {
    if (motorStates[i] == 1) {
      motor_st = 'R';
      break;
    }
  }
  data += motor_st;
  Serial.println(data);  

}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != 2) return;

  uint8_t board = data[0];
  uint8_t status = data[1];

  uint8_t ack = 0xAA;
  esp_now_send(mac, &ack, 1);

  if (board == 1) {
    motorStates[0] = (status >> 2) & 1;
    motorStates[1] = (status >> 1) & 1;
    motorStates[2] = status & 1;
  }
  else if (board == 2) {
    motorStates[3] = (status == 1);
    motorStates[4] = (status == 1);
    motorStates[5] = (status == 2);
    motorStates[6] = (status == 2);
  } else return;

  bool changed = false;
  for (int i = 0; i < 8; i++) {   
    if (motorStates[i] != lastSentStates[i]) {
      lastSentStates[i] = motorStates[i];
      changed = true;
    }
  }

  if (changed) {
    sendCompactState();
  }
}

void setup() {
  Serial.begin(9600);
  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);

  if (esp_now_init() != 0) {
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onReceive);

  uint8_t broadcast[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_add_peer(broadcast, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
}

void loop() {
}