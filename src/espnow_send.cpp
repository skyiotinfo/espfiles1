#include <ESP8266WiFi.h>
#include <espnow.h>

#define BUTTON_PIN D9

// CHANGE THIS TO YOUR RECEIVER MAC ADDRESS
uint8_t receiverMAC[] = {0xFC, 0xF5, 0xC4, 0xBE, 0x79, 0xB3};

typedef struct {
  bool toggle;
} Message;

Message msg;

void onSend(uint8_t *mac, uint8_t status) {
  Serial.println(status == 0 ? "Sent OK" : "Send Failed");
}

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_now_init();
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSend);
  esp_now_add_peer(receiverMAC, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
}

void loop() {
  static bool lastState = HIGH;
  bool currentState = digitalRead(BUTTON_PIN);

  if (lastState == HIGH && currentState == LOW) {
    msg.toggle = true;
    esp_now_send(receiverMAC, (uint8_t *)&msg, sizeof(msg));
    Serial.println("Button Pressed → Toggle Sent");
    delay(300);   // debounce
  }

  lastState = currentState;
}
