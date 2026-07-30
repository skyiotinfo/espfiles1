#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
  #include "user_interface.h"
}

#define REFLECTOR "B"

uint8_t NEXT_HOP_TOWARD_MOTOR_MAC[6] = {0xFC, 0xF5, 0xC4, 0xBE, 0x5D, 0xD9}; // real motor board MAC

const uint8_t NUM_NODES = 4;


uint8_t RELAY_SLAVE_MAC[NUM_NODES + 1][6] = {
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0},
    {0x50, 0x02, 0x91, 0xDD, 0x6B, 0xD7}, // Reflector A's MAC (node 3, via A)
    {0x50, 0x02, 0x91, 0xDD, 0x6B, 0xD7}, // Reflector A's MAC (node 4, via A)
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
  Serial.printf("🔁 Slave -> Motor : forwarding signal %04d (via Reflector A)\n", code);
}

void forwardToSlave(uint8_t id, uint16_t code) {
  if (id < 1 || id > NUM_NODES || macIsZero(RELAY_SLAVE_MAC[id])) {
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
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
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
  Serial.println(F("^ Point the motor board's slaveMac[3]/[4] entries at THIS address."));
  Serial.println(F("  Also update Reflector A's NEXT_HOP_TOWARD_MOTOR_MAC to point at THIS address."));

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

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
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
