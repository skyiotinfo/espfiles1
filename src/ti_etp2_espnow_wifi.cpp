
#include <Arduino.h>
#include <espnow.h>
#include <ESP8266WiFi.h>
extern "C" { 
  #include "user_interface.h" 
}

#define M3_PIN          D6
#define HIGH_SENSOR_PIN D1  
#define LOW_SENSOR_PIN  D2   
#define UT_SENSOR_PIN   D5   // LOW = tank2 empty)
#define TANK2_PIN       D8

uint8_t receiverMac[] = {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0};   
uint8_t gatewayMac[]  = {0xA4, 0xCF, 0x12, 0xED, 0xB2, 0x5F};   

const unsigned long MAX_RUNTIME_TANK2 = 5UL * 60UL * 1000UL;
const unsigned long RETRY_INTERVAL    = 300;

unsigned long lastHeartbeatSend = 0;
unsigned long lastHeartbeatAck = 0;
const unsigned long HEARTBEAT_INTERVAL = 10000;  
bool heartbeatActive = false;

enum CycleState {
  WAIT_EMPTY_START_TANK1,
  WAIT_FULL_STOP_TANK1,
  WAIT_EMPTY_STOP_M3,
  WAIT_FULL_STOP_TANK2,
  WAIT_EMPTY_RESTART_CYCLE
};
CycleState cycleState = WAIT_EMPTY_START_TANK1;

bool lastEmptyState = true;   
bool lastFullState  = true;   
bool motorAckReceived = false;
bool requestActive = false;
uint8_t requestValue = 0;

unsigned long tank2StartTime = 0;
bool tank2Running = false;
bool motorIsOn = false;

uint8_t lastAckedGatewayState = 255;
uint8_t pendingGatewayState = 0;
bool gatewayStatePending = false;
unsigned long gatewayLastSendTime = 0;

unsigned long lastDiagnosticPrint = 0;

void sendMotorCommand(uint8_t value);
void startM3();
void stopMotors();
void startTank2();
void stopTank2();
bool isUTLowConfirmed();
void trySendPendingGatewayState();
void allOffSafe();
void handleHeartbeat();

void onSent(uint8_t *mac_addr, uint8_t sendStatus) {
}

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
  uint8_t value = data[0];
  if (value == 2) {
    motorAckReceived = true;
    requestActive = false;
    if (requestValue == 1) motorIsOn = true;
    else if (requestValue == 0) motorIsOn = false;
    lastHeartbeatAck = millis();
    Serial.printf("✅ Motor ACK received, motorIsOn=%d\n", motorIsOn);
  }
  if (len == 1 && value == 0xAA) {
    lastAckedGatewayState = pendingGatewayState;
    gatewayStatePending = false;
    Serial.printf("✅ Gateway ACK received for state %d\n", pendingGatewayState);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n🔧 MASTER BOOTING...");

  pinMode(M3_PIN, OUTPUT);
  pinMode(TANK2_PIN, OUTPUT);
  pinMode(HIGH_SENSOR_PIN, INPUT_PULLUP);
  pinMode(LOW_SENSOR_PIN,  INPUT_PULLUP);
  pinMode(UT_SENSOR_PIN,   INPUT_PULLUP);

  digitalWrite(M3_PIN, HIGH);
  digitalWrite(TANK2_PIN, LOW);

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(receiverMac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  esp_now_add_peer(gatewayMac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
}

void loop() {
  static unsigned long lastRead = 0;
  static unsigned long lastUTCheck = 0;

  if (millis() - lastDiagnosticPrint > 5000) {
    lastDiagnosticPrint = millis();
    bool rawEmpty = digitalRead(LOW_SENSOR_PIN);
    bool rawFull  = digitalRead(HIGH_SENSOR_PIN);
    bool emptyNow = (rawEmpty == LOW);
    bool fullNow  = (rawFull == LOW);
    Serial.printf("📊 Sensors: EMPTY=%d, FULL=%d | State=%d | Tank1=%d, M3=%d, Tank2=%d\n",
                  emptyNow, fullNow, cycleState, motorIsOn, digitalRead(M3_PIN), tank2Running);
  }

  if (millis() - lastRead > 200) {
    bool rawFull  = digitalRead(HIGH_SENSOR_PIN);
    bool rawEmpty = digitalRead(LOW_SENSOR_PIN);
    bool fullNow  = (rawFull == LOW);
    bool emptyNow = (rawEmpty == LOW);
    lastRead = millis();

    bool emptyChanged = (emptyNow != lastEmptyState);
    bool fullChanged  = (fullNow != lastFullState);

    if (emptyChanged || fullChanged) {
      bool tankEmptyNow = emptyNow;
      bool tankFullNow  = fullNow;

      Serial.printf("🔁 Sensor change: empty=%d (was %d), full=%d (was %d)\n",
                    tankEmptyNow, lastEmptyState, tankFullNow, lastFullState);
      switch (cycleState) {
        case WAIT_EMPTY_START_TANK1:
          if (tankEmptyNow) {
            Serial.println("EMPTY → START TANK1");
            allOffSafe();
            sendMotorCommand(1);
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
            if (!isUTLowConfirmed()) {
              startTank2();
              cycleState = WAIT_FULL_STOP_TANK2;
            } else {
              sendMotorCommand(1);
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
            sendMotorCommand(1);
            cycleState = WAIT_FULL_STOP_TANK1;
          }
          break;
      }
      lastEmptyState = emptyNow;
      lastFullState  = fullNow;
    }
  }

  bool tank2Low = false;
  if (millis() - lastUTCheck > 100) {
    lastUTCheck = millis();
    tank2Low = isUTLowConfirmed();
  }
  if (tank2Low && !requestActive) {
    Serial.println("⚠️ UT LOW → FORCE TANK1");
    allOffSafe();
    sendMotorCommand(1);
    cycleState = WAIT_FULL_STOP_TANK1;
    lastEmptyState = (digitalRead(LOW_SENSOR_PIN) == LOW);
    lastFullState  = (digitalRead(HIGH_SENSOR_PIN) == LOW);
  }

  if (tank2Running && (millis() - tank2StartTime > MAX_RUNTIME_TANK2)) {
    Serial.println("⚠️ Tank2 TIMEOUT – forcing stop");
    allOffSafe();
    startM3();
    cycleState = WAIT_EMPTY_RESTART_CYCLE;
  }

  if (requestActive && !motorAckReceived) {
    static unsigned long lastSendTime = 0;
    if (millis() - lastSendTime >= 200) {
      lastSendTime = millis();
      esp_now_send(receiverMac, &requestValue, sizeof(requestValue));
      Serial.printf("🔄 Retrying motor command: %d\n", requestValue);
    }
  }

  uint8_t currentState = ((motorIsOn ? 1 : 0) << 2) |
                         ((digitalRead(TANK2_PIN) ? 1 : 0) << 1) |
                         (digitalRead(M3_PIN) ? 1 : 0);
  if (currentState != lastAckedGatewayState && !gatewayStatePending) {
    pendingGatewayState = currentState;
    gatewayStatePending = true;
    gatewayLastSendTime = 0;
  }
  trySendPendingGatewayState();

  handleHeartbeat();

  delay(5);
}

void sendMotorCommand(uint8_t value) {
  requestValue = value;
  requestActive = true;
  motorAckReceived = false;
  esp_now_send(receiverMac, &value, sizeof(value));
  Serial.printf("📤 Sent motor command: %d\n", value);
  if (value == 1) {
    heartbeatActive = true;
    lastHeartbeatSend = millis();
    lastHeartbeatAck = millis();
    Serial.println("❤️ Heartbeat activated");
  } else if (value == 0) {
    heartbeatActive = false;
    Serial.println("❤️ Heartbeat deactivated");
  }
}

void startM3() {
  digitalWrite(M3_PIN, HIGH);
  Serial.println("▶️ M3 started");
}

void stopMotors() {
  digitalWrite(M3_PIN, LOW);
  Serial.println("⏹️ M3 stopped");
}

void startTank2() {
  digitalWrite(TANK2_PIN, HIGH);
  tank2StartTime = millis();
  tank2Running = true;
  Serial.println("▶️ Tank2 started");
}

void stopTank2() {
  digitalWrite(TANK2_PIN, LOW);
  tank2Running = false;
  Serial.println("⏹️ Tank2 stopped");
}

bool isUTLowConfirmed() {
  int count = 0;
  for (int i = 0; i < 5; i++) {
    if (digitalRead(UT_SENSOR_PIN) == LOW) count++;
    delay(2);
  }
  bool low = (count >= 4);

  return low;
}

void trySendPendingGatewayState() {
  if (!gatewayStatePending) return;
  if (millis() - gatewayLastSendTime >= RETRY_INTERVAL) {
    gatewayLastSendTime = millis();
    uint8_t data[2] = {1, pendingGatewayState};
    esp_now_send(gatewayMac, data, sizeof(data));
    Serial.printf("📤 Sending to Gateway: board=1, state=%d\n", pendingGatewayState);
  }
}

void handleHeartbeat() {
  if (!heartbeatActive) return;
  unsigned long now = millis();
  if (now - lastHeartbeatSend >= HEARTBEAT_INTERVAL) {
    lastHeartbeatSend = now;
    sendMotorCommand(1);
    Serial.println("❤️ Heartbeat: resending motor start");
  }
  const unsigned long HEARTBEAT_TIMEOUT = 15000;
  if (now - lastHeartbeatAck >= HEARTBEAT_TIMEOUT) {
    Serial.println("💀 Heartbeat timeout! No ACK for 15 seconds.");
    motorIsOn = false;
    heartbeatActive = false;
    allOffSafe();
    cycleState = WAIT_EMPTY_START_TANK1;
    lastEmptyState = digitalRead(LOW_SENSOR_PIN) == LOW;
    lastFullState  = digitalRead(HIGH_SENSOR_PIN) == LOW;
    Serial.println("System reset due to heartbeat timeout.");
  }
}

void allOffSafe() {
  digitalWrite(M3_PIN, LOW);
  digitalWrite(TANK2_PIN, LOW);
  tank2Running = false;
  Serial.println("🛑 All outputs OFF");
  sendMotorCommand(0);
  unsigned long start = millis();
  while (!motorAckReceived && millis() - start < 1000) {
    delay(10);
  }
  if (!motorAckReceived) {
    motorIsOn = false;
    Serial.println("⚠️ No ACK – forcing motorIsOn=false");
  }
  motorAckReceived = false;
  delay(200);
}