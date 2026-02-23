#include <ESP8266WiFi.h>
#include <espnow.h>

#define LED_PIN D6

bool ledState = false;

typedef struct {
  bool toggle;
} Message;

Message msg;

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  memcpy(&msg, data, sizeof(msg));

  if (msg.toggle) {
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
    Serial.println(ledState ? "LED ON" : "LED OFF");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_now_init();
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(onReceive);
}

void loop() {
  // nothing here
}
