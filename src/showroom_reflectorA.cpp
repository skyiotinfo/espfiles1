#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
  #include "user_interface.h"
}

#define REFLECTOR "A"


uint8_t NEXT_HOP_TOWARD_MOTOR_MAC[6] = {0xFC, 0xF5, 0xC4, 0xBE, 0xD2, 0xDC}; // <-- Reflector B's MAC fc:f5:c4:be:d2:dc


const uint8_t NODE_ID_MIN = 3;
const uint8_t NODE_ID_MAX = 4;


uint8_t RELAY_SLAVE_MAC[NODE_ID_MAX + 1][6] = {
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0},
    {0xFC, 0xF5, 0xC4, 0xBE, 0xBF, 0x31}, // node 3's  board MAC
    {0xFC, 0xF5, 0xC4, 0xBE, 0x6D, 0xCE}, // node 4's  board MAC
};

volatile uint16_t rxQueue[16];
volatile uint8_t  rxHead = 0;
volatile bool     rxAvailable = false;

unsigned long forwardedToMotor = 0;
unsigned long forwardedToSlave = 0;
unsigned long droppedUnroutable = 0;


bool macIsZero(uint8_t *mac);
void forwardToMotor(uint16_t code);
void forwardToSlave(uint8_t id, uint16_t code);
void handleIncoming(uint16_t code);
void printStatus();


bool macIsZero(uint8_t *mac) {
  for (uint8_t i = 0; i < 6; i++) {
    if (mac[i] != 0) return false;
  }
  return true;
}

void forwardToMotor(uint16_t code) {
  esp_now_send(NEXT_HOP_TOWARD_MOTOR_MAC, (uint8_t *)&code, sizeof(code));
  forwardedToMotor++;
  Serial.printf("🔁 Slave -> Motor : forwarding signal %04d\n", code);
}

void forwardToSlave(uint8_t id, uint16_t code) {
  if (id < NODE_ID_MIN || id > NODE_ID_MAX || macIsZero(RELAY_SLAVE_MAC[id])) {
    Serial.printf("⚠️  No relay mapping configured for node %d — dropping signal %04d\n", id, code);
    droppedUnroutable++;
    return;
  }
  esp_now_send(RELAY_SLAVE_MAC[id], (uint8_t *)&code, sizeof(code));
  forwardedToSlave++;
  Serial.printf("🔁 Motor -> Node %d : forwarding signal %04d\n", id, code);
}

void handleIncoming(uint16_t code) {
  uint8_t header = code / 1000;
  uint8_t id     = (code / 100) % 10;

  if (header == 2) {
    forwardToMotor(code);
  } else if (header == 3) {
    forwardToSlave(id, code);
  } else {
    Serial.printf("⚠️  Unrecognized signal header, ignoring: %04d\n", code);
  }
}


void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 2 || rxHead >= 16) return;
  uint16_t signal = 0;
  memcpy(&signal, data, sizeof(uint16_t));
  rxQueue[rxHead++] = signal;
  rxAvailable = true;
}

void onSent(uint8_t *mac, uint8_t status) {}

void handleQueued() {
  if (!rxAvailable) return;

  noInterrupts();
  uint8_t count = rxHead;
  rxHead = 0;
  interrupts();

  for (uint8_t i = 0; i < count; i++) {
    handleIncoming(rxQueue[i]);
  }
  rxAvailable = false;
}

void printStatus() {
  static unsigned long last = 0;
  if (millis() - last < 10000) return;
  last = millis();

  Serial.println(F("\n──────────────────────────────────────"));
  Serial.printf("🔁 Relay [%s] stats — to motor: %lu | to slaves: %lu | dropped (unmapped): %lu\n",
                REFLECTOR, forwardedToMotor, forwardedToSlave, droppedUnroutable);
  Serial.print(F("🗺️  Relaying for nodes: "));
  bool any = false;
  for (uint8_t i = NODE_ID_MIN; i <= NODE_ID_MAX; i++) {
    if (!macIsZero(RELAY_SLAVE_MAC[i])) {
      Serial.printf("%d ", i);
      any = true;
    }
  }
  if (!any) Serial.print(F("(none configured)"));
  Serial.println();
  Serial.println(F("──────────────────────────────────────\n"));
}


void setup() {
  Serial.begin(115200);
  Serial.printf("\n🌱 Relay Node [%s] booting...\n", REFLECTOR);

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1); 

  Serial.print(F("This relay's MAC: "));
  Serial.println(WiFi.macAddress());
  Serial.println(F("^ Point the relayed slave's motorMac[] AND the motor board's"));
  Serial.println(F("  matching slaveMac[id] entry at THIS address, not at each other."));

  if (esp_now_init() != 0) {
    Serial.println(F("❌ ESP-NOW init failed"));
    return;
  }


#ifdef ESP_NOW_ROLE_COMBO
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
#else
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
#endif

  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);

  esp_now_add_peer(NEXT_HOP_TOWARD_MOTOR_MAC, ESP_NOW_ROLE_CONTROLLER, 1, NULL, 0);

  for (uint8_t i = NODE_ID_MIN; i <= NODE_ID_MAX; i++) {
    if (!macIsZero(RELAY_SLAVE_MAC[i])) {
      esp_now_add_peer(RELAY_SLAVE_MAC[i], ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
      Serial.printf("✅ Relaying configured for node %d\n", i);
    }
  }

  Serial.println(F("✅ Relay Node ready\n"));
}

void loop() {
  handleQueued();
  printStatus();
  delay(50);
}
