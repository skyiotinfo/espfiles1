#include <Arduino.h>
#include <espnow.h>
#include <ESP8266WiFi.h>
extern "C" { 
  #include "user_interface.h" 
}

// ========== PIN DEFINITIONS ==========
#define M3_PIN        D6
#define FLOAT_SENSOR  D1
#define UT_SENSOR     D2
#define TANK2_PIN     D8

// ========== ESP-NOW ==========
uint8_t receiverMac[] = {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0};   // Motor board
uint8_t board4Mac[]   = {0x3C, 0x61, 0x05, 0xDC, 0x6A, 0x29};   // Board4

// ========== TIMING ==========
const unsigned long MAX_RUNTIME_TANK2 = 20UL * 60UL * 1000UL;
const unsigned long RETRY_INTERVAL    = 300;      // resend to Board4 every 300ms until ACK

// ========== STATE MACHINE ==========
enum CycleState {
  WAIT_EMPTY_START_TANK1,
  WAIT_FULL_STOP_TANK1,
  WAIT_EMPTY_STOP_M3,
  WAIT_FULL_STOP_TANK2,
  WAIT_EMPTY_RESTART_CYCLE
};
CycleState cycleState = WAIT_EMPTY_START_TANK1;

// ========== GLOBAL ==========
bool lastFloatState = true;
bool motorAckReceived = false;
bool requestActive = false;
uint8_t requestValue = 0;

unsigned long tank2StartTime = 0;
bool tank2Running = false;

// ---------- Board4 (send until ACK) ----------
uint8_t lastAckedBoard4State = 255;
uint8_t pendingBoard4State = 0;
bool board4StatePending = false;
unsigned long board4LastSendTime = 0;

// ========== FUNCTION PROTOTYPES ==========
void sendESPNow(uint8_t value);
void startM3();
void stopMotors();
void startTank2();
void stopTank2();
bool isUTLowConfirmed();
void trySendPendingBoard4State();
void allOffSafe();

// ========== ESP-NOW CALLBACKS ==========
void onSent(uint8_t *mac_addr, uint8_t sendStatus) {}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  uint8_t value = data[0];

  // ACK from motor board
  if (value == 2) {
    motorAckReceived = true;
    requestActive = false;
  }

  // ACK from Board4 (0xAA = acknowledge)
  if (len == 1 && value == 0xAA) {
    lastAckedBoard4State = pendingBoard4State;
    board4StatePending = false;   // ✅ stop sending because ACK received
    Serial.println("✅ Board4 ACK received - stopping retries");
  }
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);

  pinMode(M3_PIN, OUTPUT);
  pinMode(TANK2_PIN, OUTPUT);
  pinMode(FLOAT_SENSOR, INPUT_PULLUP);
  pinMode(UT_SENSOR, INPUT_PULLUP);

  digitalWrite(M3_PIN, LOW);
  digitalWrite(TANK2_PIN, LOW);

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);

  esp_now_init();
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(receiverMac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  esp_now_add_peer(board4Mac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  Serial.println("✅ SYSTEM READY (Board4 retry until ACK)");
}

// ========== MAIN LOOP ==========
void loop() {
  static unsigned long lastRead = 0;
  static unsigned long lastUTCheck = 0;

  bool tankEmptyNow = false, tankFullNow = false;

  if (millis() - lastRead > 200) {
    bool floatReading = digitalRead(FLOAT_SENSOR);
    tankFullNow = floatReading;
    tankEmptyNow = !floatReading;
    lastRead = millis();
  } else return;

  bool tank2Low = false;
  if (millis() - lastUTCheck > 100) {
    lastUTCheck = millis();
    tank2Low = isUTLowConfirmed();
  }

  // ================== UT OVERRIDE ==================
  if (tank2Low && !requestActive) {
    Serial.println("⚠️ UT LOW → FORCE TANK1");
    allOffSafe();
    sendESPNow(1);
    cycleState = WAIT_FULL_STOP_TANK1;
    lastFloatState = tankFullNow;
    return;
  }

  // ================== STATE MACHINE ==================
  if (tankEmptyNow != !lastFloatState) {
    switch (cycleState) {
      case WAIT_EMPTY_START_TANK1:
        if (tankEmptyNow) {
          Serial.println("EMPTY → START TANK1");
          allOffSafe();
          sendESPNow(1);
          cycleState = WAIT_FULL_STOP_TANK1;
        }
        break;
      case WAIT_FULL_STOP_TANK1:
        if (tankFullNow) {
          Serial.println("FULL → STOP TANK1 → START M3");
          allOffSafe();
          startM3();
          cycleState = WAIT_EMPTY_STOP_M3;
        }
        break;
      case WAIT_EMPTY_STOP_M3:
        if (tankEmptyNow) {
          Serial.println("EMPTY → STOP M3 → START TANK2");
          allOffSafe();
          if (!tank2Low) {
            startTank2();
            cycleState = WAIT_FULL_STOP_TANK2;
          } else {
            sendESPNow(1);
            cycleState = WAIT_FULL_STOP_TANK1;
          }
        }
        break;
      case WAIT_FULL_STOP_TANK2:
        if (tankFullNow) {
          Serial.println("FULL → STOP TANK2 → START M3");
          allOffSafe();
          startM3();
          cycleState = WAIT_EMPTY_RESTART_CYCLE;
        }
        break;
      case WAIT_EMPTY_RESTART_CYCLE:
        if (tankEmptyNow) {
          Serial.println("EMPTY → RESTART CYCLE (TANK1)");
          allOffSafe();
          sendESPNow(1);
          cycleState = WAIT_FULL_STOP_TANK1;
        }
        break;
    }
    lastFloatState = tankFullNow;
  }

  // ========== SAFETY TIMEOUT ==========
  if (tank2Running && (millis() - tank2StartTime > MAX_RUNTIME_TANK2)) {
    Serial.println("⚠️ Tank2 TIMEOUT");
    allOffSafe();
    startM3();
    cycleState = WAIT_EMPTY_RESTART_CYCLE;
  }

  // Retry motor command if no ACK
  if (requestActive && !motorAckReceived) {
    static unsigned long lastSendTime = 0;
    if (millis() - lastSendTime >= 200) {
      lastSendTime = millis();
      esp_now_send(receiverMac, &requestValue, sizeof(requestValue));
    }
  }

  // ========== BOARD4: SEND STATE UNTIL ACK ==========
  // Build current state: bit2 = float full, bit1 = tank2, bit0 = M3
  uint8_t currentState = ((tankFullNow ? 1 : 0) << 2) |
                         ((digitalRead(TANK2_PIN) ? 1 : 0) << 1) |
                         (digitalRead(M3_PIN) ? 1 : 0);
  
  // If state changed and no pending message, start sending
  if (currentState != lastAckedBoard4State && !board4StatePending) {
    pendingBoard4State = currentState;
    board4StatePending = true;
    board4LastSendTime = 0;
    Serial.print("📤 New state for Board4 (will retry until ACK): ");
    Serial.println(currentState);
  }

  // This function sends every 300ms until board4StatePending becomes false (ACK received)
  trySendPendingBoard4State();

  delay(5);
}

// ========== HELPER FUNCTIONS ==========
void sendESPNow(uint8_t value) {
  requestValue = value;
  requestActive = true;
  motorAckReceived = false;
}

void startM3() {
  digitalWrite(M3_PIN, HIGH);
}

void stopMotors() {
  digitalWrite(M3_PIN, LOW);
}

void startTank2() {
  digitalWrite(TANK2_PIN, HIGH);
  tank2StartTime = millis();
  tank2Running = true;
}

void stopTank2() {
  digitalWrite(TANK2_PIN, LOW);
  tank2Running = false;
}

bool isUTLowConfirmed() {
  int count = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(UT_SENSOR) == LOW) count++;
    delay(2);
  }
  return count >= 4;
}

// Sends the pending Board4 state every 300ms until ACK clears the flag
void trySendPendingBoard4State() {
  if (!board4StatePending) return;
  if (millis() - board4LastSendTime >= RETRY_INTERVAL) {
    board4LastSendTime = millis();
    uint8_t data[2] = {1, pendingBoard4State};
    int result = esp_now_send(board4Mac, data, sizeof(data));
    if (result == 0) {
      Serial.print("📤 Sending to Board4 (retry until ACK): ");
      Serial.println(pendingBoard4State);
    } else {
      Serial.println("❌ Send to Board4 failed");
    }
  }
}

// Blocking safe-off (waits for motor ACK)
void allOffSafe() {
  digitalWrite(M3_PIN, LOW);
  digitalWrite(TANK2_PIN, LOW);
  tank2Running = false;

  sendESPNow(0);   // stop motor

  unsigned long start = millis();
  while (!motorAckReceived && millis() - start < 1000) {
    delay(10);
  }
  motorAckReceived = false;
  delay(200);
}