#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
  #include "user_interface.h"
}

const uint8_t PUMP_PIN  = D8;

const uint8_t DOOR_PIN[3] = {D1, D2};
const uint8_t NUM_SAFETY_PINS = 3;
const unsigned long INTERLOCK_WARN_INTERVAL_MS = 2000;

const uint8_t NUM_NODES = 4;


const uint8_t DOOR_INDEX_FOR_NODE[NUM_NODES + 1] = {0, 0, 1, 2, 2};
const char *doorNameForNode(uint8_t id) {
  static const char *names[NUM_NODES + 1] = {"", "D1", "D2", "D6", "D6"};
  return names[id];
}
bool doorOpenForNode(uint8_t id) {
  return digitalRead(DOOR_PIN[DOOR_INDEX_FOR_NODE[id]]) == HIGH;
}


const uint8_t SOURCE_EMPTY_PIN = D5;
bool sourceEmptyConnected() {
  return digitalRead(SOURCE_EMPTY_PIN) == LOW;
}

const unsigned long SOURCE_EMPTY_CONFIRM_MS = 5000UL;
bool          prevSourceEmptyRaw       = false;
unsigned long sourceEmptySustainedSince = 0; 

void updateSourceEmptyTracking() {
  bool raw = sourceEmptyConnected();
  if (raw) {
    if (!prevSourceEmptyRaw) sourceEmptySustainedSince = millis(); 
  } else {
    sourceEmptySustainedSince = 0; 
  }
  prevSourceEmptyRaw = raw;
}

bool sourceEmptyConfirmed() {
  return prevSourceEmptyRaw && sourceEmptySustainedSince != 0 &&
         (millis() - sourceEmptySustainedSince >= SOURCE_EMPTY_CONFIRM_MS);
}

uint8_t slaveMac[NUM_NODES + 1][6] = {
    {0, 0, 0, 0, 0, 0},
    {0x98, 0xF4, 0xAB, 0xD8, 0xDD, 0xED},
    //{0x48, 0x55, 0x19, 0xEC, 0xAA, 0xCF},
    {0xCC, 0x50, 0xE3, 0x6B, 0x42, 0x85},
    {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0}, // 50:02:91:dd:6b:d7
    {0xFC, 0xF5, 0xC4, 0xBE, 0xD2, 0xDC}, // 50:02:91:dd:6b:d7
};

const unsigned long PUMP_STOP_SETTLE_MS  = 600;
const unsigned long VALVE_OPEN_SETTLE_MS = 300;


const unsigned long NODE_SILENCE_TRIGGER_MS = 8000UL; 
const uint8_t       WATCHDOG_COUNTDOWN_START = 5;     
const unsigned long WATCHDOG_TICK_MS         = 50UL;


uint8_t STATUS_BOARD_MAC[6] = {0x68, 0xC6, 0x3A, 0xF1, 0x51, 0x7E}; //3c:61:05:dc:6a:29
const unsigned long STATUS_SEND_INTERVAL_MS = 8000UL;

struct StatusPacket {
  uint8_t  pumpOn;             
  uint8_t  doorOpen[3];         
  uint8_t  interlockOk;         
  uint8_t  nodeOpen[5];        
  uint8_t  nodeEverSeen[5];     
  uint16_t nodeLastSeenSec[5];  
};

bool statusPeerAdded = false;

void broadcastStatus();


bool          confirmedOpen[NUM_NODES + 1] = {false, false, false, false, false};
bool          pumpRunning                  = false;
unsigned long lastSeenAt[NUM_NODES + 1]    = {0, 0, 0, 0, 0};
bool          everSeen[NUM_NODES + 1]      = {false, false, false, false, false};

bool          pumpStartPending = false;
unsigned long pumpStartAt      = 0;

bool          nodeClosePending[NUM_NODES + 1] = {false, false, false, false, false};
unsigned long nodeCloseAt[NUM_NODES + 1]       = {0, 0, 0, 0, 0};

unsigned long lastInterlockWarnAt = 0;

bool          watchdogActive[NUM_NODES + 1]      = {false, false, false, false, false};
int8_t        watchdogCounter[NUM_NODES + 1]     = {0, 0, 0, 0, 0};
unsigned long watchdogLastTickAt[NUM_NODES + 1]  = {0, 0, 0, 0, 0};

volatile uint16_t rxQueue[16];
volatile uint8_t  rxHead = 0;
volatile bool     rxAvailable = false;


bool interlockOk();
void sendCommand(uint8_t id, bool openIt);
uint8_t countOpenExcluding(uint8_t excludeId);
uint8_t countOpen();
void startPump();
void stopPump();
void handleOpenRequest(uint8_t id);
void handleCloseRequest(uint8_t id);
void handleIncoming(uint16_t signal);
void processPending();
void checkNodeWatchdog();
void checkDoorAuthorization(); 
void updateSourceEmptyTracking(); 
void printStatus();


bool interlockOk() {
  for (uint8_t i = 0; i < NUM_SAFETY_PINS; i++) {
    if (digitalRead(DOOR_PIN[i]) == HIGH) return true;
  }
  return false;
}

void sendCommand(uint8_t id, bool openIt) {
  uint16_t code = 3000 + (id * 100) + (openIt ? 1 : 0);
  esp_now_send(slaveMac[id], (uint8_t *)&code, sizeof(code));
  Serial.printf("📤 -> Node %d : %s\n", id, openIt ? "OPEN approved" : "CLOSE approved");
}

uint8_t countOpenExcluding(uint8_t excludeId) {
  uint8_t n = 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (i != excludeId && confirmedOpen[i]) n++;
  }
  return n;
}

uint8_t countOpen() {
  return countOpenExcluding(0);
}

void startPump() {
  if (!pumpRunning) {
    digitalWrite(PUMP_PIN, HIGH);
    pumpRunning = true;
    Serial.println("🔌 PUMP ON");
  }
}

void stopPump() {
  if (pumpRunning) {
    digitalWrite(PUMP_PIN, LOW);
    pumpRunning = false;
    Serial.println("⏹️  PUMP OFF");
  }
}

void handleOpenRequest(uint8_t id) {
  Serial.printf("📥 Node %d requests OPEN\n", id);

  watchdogActive[id] = false;

  if (confirmedOpen[id]) {
    sendCommand(id, true);
    return;
  }

  if (!doorOpenForNode(id)) {
    Serial.printf("⛔ Node %d OPEN request BLOCKED — its door (%s) is closed\n", id, doorNameForNode(id));
    return;
  }

  confirmedOpen[id] = true;
  sendCommand(id, true);

  pumpStartPending = true;
  pumpStartAt = millis() + VALVE_OPEN_SETTLE_MS;
}

void handleCloseRequest(uint8_t id) {

  watchdogActive[id] = false;

  if (!confirmedOpen[id]) {
    sendCommand(id, false);
    return;
  }

  uint8_t others = countOpenExcluding(id);

  if (others == 0) {
    stopPump();
    nodeClosePending[id] = true;
    nodeCloseAt[id] = millis() + PUMP_STOP_SETTLE_MS;
  } else {
    confirmedOpen[id] = false;
    sendCommand(id, false);
  }
}

void handleIncoming(uint16_t signal) {
  uint8_t header = signal / 1000;
  if (header != 2) {
    Serial.printf("⚠️  Ignoring non-request signal: %d\n", signal);
    return;
  }

  uint8_t id  = (signal / 100) % 10;
  uint16_t sub = signal % 100;

  if (id < 1 || id > NUM_NODES) {
    Serial.printf("⚠️  Bad node id in signal: %d\n", signal);
    return;
  }

  lastSeenAt[id] = millis();
  everSeen[id]   = true;

  if (sub == 11) {
    handleOpenRequest(id);
  } else if (sub == 0) {
    handleCloseRequest(id);
  } else {
    Serial.printf("⚠️  Unknown request subcode in signal: %d\n", signal);
  }
}

void processPending() {
  if (pumpStartPending && (long)(millis() - pumpStartAt) >= 0) {
    if (countOpen() > 0) {
      if (interlockOk() && !sourceEmptyConfirmed()) {
        startPump();
        pumpStartPending = false;
      } else {
        if (millis() - lastInterlockWarnAt > INTERLOCK_WARN_INTERVAL_MS) {
          if (sourceEmptyConfirmed()) {
            Serial.println("⛔ Pump start BLOCKED — D7 source-empty confirmed connected for 5s+");
          } else {
            Serial.println("⛔ Pump start BLOCKED — all 3 doors are OPEN (D1,D2,D5 all HIGH)");
          }
          lastInterlockWarnAt = millis();
        }
      }
    } else {
      pumpStartPending = false;
    }
  }

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (nodeClosePending[i] && (long)(millis() - nodeCloseAt[i]) >= 0) {
      confirmedOpen[i] = false;
      sendCommand(i, false);
      nodeClosePending[i] = false;
      Serial.println("═══════════════════════════════════════════");
    }
  }
}


void checkNodeWatchdog() {
  for (uint8_t i = 1; i <= NUM_NODES; i++) {

    if (!confirmedOpen[i] || !everSeen[i]) {
      watchdogActive[i] = false; 
      continue;
    }

    unsigned long silence = millis() - lastSeenAt[i];

    if (silence <= NODE_SILENCE_TRIGGER_MS) {
      if (watchdogActive[i]) {
        Serial.printf("✅ Node %d responded again — shutdown countdown cancelled\n", i);
      }
      watchdogActive[i] = false;
      continue;
    }

    if (!watchdogActive[i]) {
      watchdogActive[i]     = true;
      watchdogCounter[i]    = WATCHDOG_COUNTDOWN_START;
      watchdogLastTickAt[i] = millis();
      Serial.printf("⚠️  Node %d silent for over %lus with valve OPEN — pump shutdown in %d...\n",
                    i, NODE_SILENCE_TRIGGER_MS / 1000, watchdogCounter[i]);
      continue;
    }

    if (millis() - watchdogLastTickAt[i] >= WATCHDOG_TICK_MS) {
      watchdogCounter[i]--;
      watchdogLastTickAt[i] = millis();

      if (watchdogCounter[i] > 0) {
        Serial.printf("⏳ Node %d still silent — marking closed in %d...\n", i, watchdogCounter[i]);
      } else {
        confirmedOpen[i] = false;
        watchdogActive[i] = false;

        if (countOpen() == 0) {
          Serial.printf("🛑 Node %d silence countdown expired -> it was the LAST open valve -> stopping pump\n", i);
          stopPump();
        } else {
          Serial.printf("🔕 Node %d silence countdown expired -> marked closed, but other valves still open -> pump continues uninterrupted\n", i);
        }
      }
    }
  }
}


void checkDoorAuthorization() {
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    if (confirmedOpen[i] && !doorOpenForNode(i)) {
      handleCloseRequest(i);
    }
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
  Serial.printf("🔌 Pump  : %s\n", pumpRunning ? "ON" : "OFF");
  Serial.printf("🚪 Doors : D1=%s  D2=%s  D5=%s  -> interlock %s\n",
                digitalRead(DOOR_PIN[0]) == HIGH ? "OPEN" : "closed",
                digitalRead(DOOR_PIN[1]) == HIGH ? "OPEN" : "closed",
                digitalRead(DOOR_PIN[2]) == HIGH ? "OPEN" : "closed",
                interlockOk() ? "OK" : "BLOCKED");
  if (sourceEmptyConfirmed()) {
    Serial.printf("🚰 Source empty (D7) : CONFIRMED connected for %lus+ (blocking start)\n", SOURCE_EMPTY_CONFIRM_MS / 1000);
  } else if (prevSourceEmptyRaw) {
    unsigned long heldFor = (millis() - sourceEmptySustainedSince) / 1000;
    Serial.printf("🚰 Source empty (D7) : connected, confirming... (%lus / %lus)\n", heldFor, SOURCE_EMPTY_CONFIRM_MS / 1000);
  } else {
    Serial.println("🚰 Source empty (D7) : not connected");
  }
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    String seenStr = everSeen[i] ? (String((millis() - lastSeenAt[i]) / 1000) + "s ago") : "never";
    if (watchdogActive[i]) {
      Serial.printf("🚿 Node %d : %s   (last seen %s)   ⏳ marking closed in %d\n",
                    i, confirmedOpen[i] ? "OPEN" : "closed", seenStr.c_str(), watchdogCounter[i]);
    } else {
      Serial.printf("🚿 Node %d : %s   (last seen %s)\n",
                    i, confirmedOpen[i] ? "OPEN" : "closed", seenStr.c_str());
    }
  }
  Serial.println(F("──────────────────────────────────────\n"));
}


void broadcastStatus() {
  static unsigned long lastSent = 0;
  if (millis() - lastSent < STATUS_SEND_INTERVAL_MS) return;
  lastSent = millis();

  StatusPacket pkt;
  pkt.pumpOn = pumpRunning ? 1 : 0;
  for (uint8_t i = 0; i < NUM_SAFETY_PINS; i++) {
    pkt.doorOpen[i] = (digitalRead(DOOR_PIN[i]) == HIGH) ? 1 : 0;
  }
  pkt.interlockOk = interlockOk() ? 1 : 0;
  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    pkt.nodeOpen[i]     = confirmedOpen[i] ? 1 : 0;
    pkt.nodeEverSeen[i] = everSeen[i] ? 1 : 0;
    pkt.nodeLastSeenSec[i] = everSeen[i]
      ? (uint16_t)((millis() - lastSeenAt[i]) / 1000)
      : 0xFFFF; 
  }

  esp_now_send(STATUS_BOARD_MAC, (uint8_t *)&pkt, sizeof(pkt));
  Serial.println("📡 Status packet sent to ESP07 monitor board");
}

void setup() {
  Serial.begin(115200);
  Serial.println(F("\n🌱 Motor Board booting..."));

  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);

  for (uint8_t i = 0; i < NUM_SAFETY_PINS; i++) {
    pinMode(DOOR_PIN[i], INPUT_PULLUP);
  }

  pinMode(SOURCE_EMPTY_PIN, INPUT_PULLUP); 

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print(F("Motor Board MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println(F("❌ ESP-NOW init failed"));
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);

  for (uint8_t i = 1; i <= NUM_NODES; i++) {
    esp_now_add_peer(slaveMac[i], ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  }


  bool statusMacIsZero = true;
  for (uint8_t i = 0; i < 6; i++) {
    if (STATUS_BOARD_MAC[i] != 0) { statusMacIsZero = false; break; }
  }
  if (!statusMacIsZero) {
    esp_now_add_peer(STATUS_BOARD_MAC, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
    statusPeerAdded = true;
    Serial.println(F("✅ Status broadcast peer (ESP07) configured"));
  } else {
    Serial.println(F("⚠️  STATUS_BOARD_MAC not set — status broadcast disabled until filled in"));
  }

  Serial.println(F("✅ Motor Board ready\n"));
}

void loop() {
  handleQueued();
  updateSourceEmptyTracking(); 
  processPending();
  checkNodeWatchdog();
  checkDoorAuthorization(); 
  printStatus();
  if (statusPeerAdded) broadcastStatus(); 
  delay(50);
}
