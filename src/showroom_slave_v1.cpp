#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
extern "C" {
  #include "user_interface.h"
}

#define DEVICE_ID 3

const uint8_t EMPTY_SENSOR_PIN  = D2;
const uint8_t FULL_SENSOR_PIN   = D1;
const uint8_t VALVE_PIN         = D8;
const bool    SENSOR_ACTIVE_LOW = true;

//uint8_t motorMac[6] = {0x48, 0x55, 0x19, 0xEC, 0xAA, 0xCF};
uint8_t motorMac[6] = {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0};
//fc:f5:c4:be:5d:d9

const unsigned long DEBOUNCE_MS             = 200;    
const unsigned long EMPTY_CONFIRM_MS        = 5000;   
const unsigned long FULL_CONFIRM_MS         = 3000;   
const unsigned long REQUEST_RETRY_MS        = 3000;   // was 3000 -- more time for a slow reply to arrive before resending
const unsigned long HEARTBEAT_MS            = 8000;  // was 8000 -- less channel traffic, more room for real requests
const unsigned long OPEN_REQUEST_TIMEOUT_MS = 120000; // was 60000 -- don't give up on a genuinely slow (not lost) reply
const unsigned long MOTOR_SILENCE_TIMEOUT_MS = 120000; // was 60000 -- same reasoning while valve is open

const unsigned long VALVE_MAX_RUN_MS  = 30UL * 60UL * 1000UL; 
const unsigned long VALVE_COOLDOWN_MS = 5UL  * 60UL * 1000UL; 


bool valveOpen = false;  

bool rawEmptyStable = false;
bool rawFullStable  = false;
bool candEmpty = false, candFull = false;
unsigned long candidateSince = 0;

bool prevEmptyStable = false;
bool prevFullStable  = false;
unsigned long emptySustainedSince = 0; 
unsigned long fullSustainedSince  = 0; 

bool wantOpenRequested  = false;
bool wantCloseRequested = false;
unsigned long lastRequestAt = 0;
unsigned long lastHeartbeat = 0;


unsigned long openRequestStreakStart = 0; 


unsigned long lastCommandReceivedAt = 0;
bool          everReceivedCommand   = false;

unsigned long valveOpenedAt      = 0; 
unsigned long valveCooldownUntil = 0; 

volatile uint16_t rxQueue[8];
volatile uint8_t  rxHead = 0;
volatile bool     rxAvailable = false;


bool readSensor(uint8_t pin);
uint16_t buildRequest(bool wantsOpen);
void sendRequest(bool wantsOpen);
void openValveLocal();
void closeValveLocal();
void handleCommand(uint16_t signal);
void updateSensors();
void evaluateAndRequest();
void checkMotorSilence();
void checkValveRunLimit(); 


bool readSensor(uint8_t pin) {
  bool raw = digitalRead(pin);
  return SENSOR_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

uint16_t buildRequest(bool wantsOpen) {
  return 2000 + (DEVICE_ID * 100) + (wantsOpen ? 11 : 0);
}

void sendRequest(bool wantsOpen) {
  uint16_t code = buildRequest(wantsOpen);
  esp_now_send(motorMac, (uint8_t *)&code, sizeof(code));
  Serial.printf("📤 Requesting %s (signal %04d)\n", wantsOpen ? "OPEN" : "CLOSE", code);
  lastRequestAt = millis();
}

void openValveLocal() {
  if (!valveOpen) {
    digitalWrite(VALVE_PIN, HIGH);
    valveOpen = true;
    valveOpenedAt = millis(); 
    Serial.println("🚿 Valve OPEN (approved by motor board)");
  }
  wantOpenRequested = false;
  openRequestStreakStart = 0; 
}

void closeValveLocal() {
  if (valveOpen) {
    digitalWrite(VALVE_PIN, LOW);
    valveOpen = false;
    valveOpenedAt = 0;                                 
    valveCooldownUntil = millis() + VALVE_COOLDOWN_MS;  
    Serial.println("🚿 Valve CLOSED (5 min cooldown before it can reopen)");
  }
  wantCloseRequested = false;
}

void handleCommand(uint16_t signal) {
  uint8_t header = signal / 1000;
  if (header != 3) return; 

  uint8_t id = (signal / 100) % 10;
  if (id != DEVICE_ID) return; 


  lastCommandReceivedAt = millis();
  everReceivedCommand   = true;

  uint16_t sub = signal % 100;
  if (sub == 1) {
    openValveLocal();
  } else if (sub == 0) {
    closeValveLocal();
  }
}

void updateSensors() {
  bool nowEmpty = readSensor(EMPTY_SENSOR_PIN);
  bool nowFull  = readSensor(FULL_SENSOR_PIN);

 
  if (nowEmpty != candEmpty || nowFull != candFull) {
    candEmpty = nowEmpty;
    candFull  = nowFull;
    candidateSince = millis();
  } else if (millis() - candidateSince >= DEBOUNCE_MS) {
    rawEmptyStable = candEmpty;
    rawFullStable  = candFull;
  }

  if (rawEmptyStable) {
    if (!prevEmptyStable) emptySustainedSince = millis();
  } else {
    emptySustainedSince = 0;
  }
  prevEmptyStable = rawEmptyStable;

  if (rawFullStable) {
    if (!prevFullStable) fullSustainedSince = millis();
  } else {
    fullSustainedSince = 0;
  }
  prevFullStable = rawFullStable;
}

void evaluateAndRequest() {
  bool emptyActive = rawEmptyStable;
  bool fullActive  = rawFullStable;

  if (emptyActive && fullActive) {
    Serial.println("⚠️  Both level sensors active at once — ignoring (check wiring)");
    return;
  }

  bool emptyConfirmed = rawEmptyStable && emptySustainedSince != 0 &&
                         (millis() - emptySustainedSince >= EMPTY_CONFIRM_MS);
  bool fullConfirmed  = rawFullStable && fullSustainedSince != 0 &&
                         (millis() - fullSustainedSince >= FULL_CONFIRM_MS);

  bool cooldownActive = (millis() < valveCooldownUntil); 
  bool needsOpen  = emptyConfirmed && !valveOpen && !cooldownActive; 
  bool needsClose = fullConfirmed  && valveOpen;

  unsigned long now = millis();
  bool retryDue     = (now - lastRequestAt >= REQUEST_RETRY_MS);
  bool heartbeatDue = (now - lastHeartbeat >= HEARTBEAT_MS);

  if (needsOpen && (!wantOpenRequested || retryDue)) {
    if (!wantOpenRequested) {
      openRequestStreakStart = now; 
    }
    wantOpenRequested  = true;
    wantCloseRequested = false;
    sendRequest(true);
  } else if (needsClose && (!wantCloseRequested || retryDue)) {
    wantCloseRequested = true;
    wantOpenRequested  = false;
    openRequestStreakStart = 0;
    sendRequest(false);
  } else if (heartbeatDue && !wantOpenRequested && !wantCloseRequested) { // FIX: don't heartbeat over a pending request
    sendRequest(valveOpen);
  }

  if (heartbeatDue) lastHeartbeat = now;

  if (wantOpenRequested && openRequestStreakStart != 0 &&
      (now - openRequestStreakStart >= OPEN_REQUEST_TIMEOUT_MS)) {
    Serial.println("⏱️  No response from motor board for 60s while requesting OPEN — keeping valve CLOSED for safety");
    closeValveLocal();
    wantOpenRequested = false;
    openRequestStreakStart = 0; 
  }
}


void checkMotorSilence() {
  if (!valveOpen || !everReceivedCommand) return; 

  if (millis() - lastCommandReceivedAt >= MOTOR_SILENCE_TIMEOUT_MS) {
    Serial.println("⏱️  No response from motor board for 60s while valve OPEN — closing valve for safety (pump status unknown)");
    closeValveLocal();
    wantOpenRequested  = false;
    wantCloseRequested = false;
    openRequestStreakStart = 0;

  }
}


void checkValveRunLimit() {
  if (!valveOpen || valveOpenedAt == 0) return;

  if (millis() - valveOpenedAt >= VALVE_MAX_RUN_MS) {
    Serial.println("⏱️  Valve has run continuously for 30 minutes — closing for a 5 min cooldown");
    closeValveLocal();
    wantOpenRequested  = false;
    wantCloseRequested = false;
    openRequestStreakStart = 0;
  }
}


void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 2 || rxHead >= 8) return;
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
    handleCommand(rxQueue[i]);
  }
  rxAvailable = false;
}


void setup() {
  Serial.begin(115200);
  Serial.printf("\n🌱 Slave Node #%d booting...\n", DEVICE_ID);

  pinMode(EMPTY_SENSOR_PIN, INPUT_PULLUP);
  pinMode(FULL_SENSOR_PIN, INPUT_PULLUP);
  pinMode(VALVE_PIN, OUTPUT);
  digitalWrite(VALVE_PIN, LOW); 

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print(F("This board's MAC: "));
  Serial.println(WiFi.macAddress());
  Serial.println(F("^ Copy this into motor_board.ino's slaveMac[] array at index = DEVICE_ID"));

  if (esp_now_init() != 0) {
    Serial.println(F("❌ ESP-NOW init failed"));
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(motorMac, ESP_NOW_ROLE_CONTROLLER, 1, NULL, 0);

  candEmpty = readSensor(EMPTY_SENSOR_PIN);
  candFull  = readSensor(FULL_SENSOR_PIN);
  rawEmptyStable = candEmpty;
  rawFullStable  = candFull;
  candidateSince = millis();

  Serial.println(F("✅ Slave Node ready\n"));
}

void loop() {
  handleQueued();
  updateSensors();
  evaluateAndRequest();
  checkMotorSilence();
  checkValveRunLimit(); 
  delay(50);
}
