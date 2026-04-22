#include <Arduino.h>
#include <ESP8266WiFi.h>
extern "C" {
#include <espnow.h>
}

// ──────────────────────────────────────────────────────────────────────────────
// Pin definitions  (adjust to match your wiring)
// ──────────────────────────────────────────────────────────────────────────────
#define MC2_MOTOR_A  D3   // Function-2 Motor A
#define MC2_MOTOR_B  D4   // Function-2 Motor B

#define MC3_MOTOR_A  D5   // Function-3 Motor A
#define MC3_MOTOR_B  D6   // Function-3 Motor B

#define WIFI_CHANNEL 1

// ──────────────────────────────────────────────────────────────────────────────
// Timing constants
// ──────────────────────────────────────────────────────────────────────────────
#define MOTOR_SWITCH_INTERVAL  300000UL   // 5 minutes in milliseconds
#define LOOP_INTERVAL            5000UL   // 5 seconds  in milliseconds

struct MotorStatusMessage {
    uint8_t nodeId;
    uint8_t messageType;
    uint8_t mc2ActiveMotor;
    uint8_t mc3ActiveMotor;
    uint32_t uptimeMs;
};

struct AckMessage {
    uint8_t nodeId;
    uint8_t messageType;
    uint8_t acknowledgedType;
    uint32_t receivedUptimeMs;
};

// ──────────────────────────────────────────────────────────────────────────────
// Per-function state  (activeMotor: 1 = Motor A ON, 2 = Motor B ON)
// ──────────────────────────────────────────────────────────────────────────────
static uint8_t  mc2_activeMotor = 1;
static uint32_t mc2_lastSwitch  = 0;

static uint8_t  mc3_activeMotor = 1;
static uint32_t mc3_lastSwitch  = 0;

// Timestamp for non-blocking 5-second loop gate
static uint32_t lastLoopRun = 0;
static uint32_t lastAckReceivedAt = 0;
static bool receiverAckedLastMessage = false;

uint8_t receiverMacAddress[] = {0x24, 0x6F, 0x28, 0x00, 0x00, 0x00};

void onDataSent(uint8_t *macAddr, uint8_t sendStatus);
void onDataReceived(uint8_t *macAddr, uint8_t *incomingData, uint8_t len);
bool initEspNow();
void sendMotorStatus();

void onDataSent(uint8_t *macAddr, uint8_t sendStatus) {
    Serial.print("ESP-NOW send status: ");
    Serial.println(sendStatus == 0 ? "delivery queued" : "delivery failed");
    receiverAckedLastMessage = false;
}

void onDataReceived(uint8_t *macAddr, uint8_t *incomingData, uint8_t len) {
    if (len != sizeof(AckMessage)) {
        Serial.println("ESP-NOW received unknown payload");
        return;
    }

    AckMessage ack;
    memcpy(&ack, incomingData, sizeof(ack));

    if (ack.messageType != 2) {
        Serial.println("ESP-NOW received non-ack payload");
        return;
    }

    receiverAckedLastMessage = true;
    lastAckReceivedAt = millis();

    Serial.print("ACK received from node ");
    Serial.print(ack.nodeId);
    Serial.print(" for type ");
    Serial.print(ack.acknowledgedType);
    Serial.print(" at uptime ");
    Serial.println(ack.receivedUptimeMs);
}

bool initEspNow() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != 0) {
        Serial.println("ESP-NOW init failed");
        return false;
    }

    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataReceived);

    if (esp_now_add_peer(receiverMacAddress, ESP_NOW_ROLE_SLAVE, WIFI_CHANNEL, NULL, 0) != 0) {
        Serial.println("ESP-NOW peer add failed");
        return false;
    }

    Serial.println("ESP-NOW ready");
    return true;
}

void sendMotorStatus() {
    MotorStatusMessage message;
    message.nodeId = 1;
    message.messageType = 1;
    message.mc2ActiveMotor = mc2_activeMotor;
    message.mc3ActiveMotor = mc3_activeMotor;
    message.uptimeMs = millis();

    uint8_t sendResult = esp_now_send(receiverMacAddress, reinterpret_cast<uint8_t *>(&message), sizeof(message));

    Serial.print("ESP-NOW send result: ");
    Serial.println(sendResult == 0 ? "ok" : "error");
}

// ──────────────────────────────────────────────────────────────────────────────
// motorControl2
// ──────────────────────────────────────────────────────────────────────────────
void motorControl2() {
    uint32_t now = millis();

    if (now - mc2_lastSwitch >= MOTOR_SWITCH_INTERVAL) {
        mc2_lastSwitch  = now;
        mc2_activeMotor = (mc2_activeMotor == 1) ? 2 : 1;
        Serial.print("[MC2] Switched to Motor ");
        Serial.println(mc2_activeMotor == 1 ? "A" : "B");
    }

    if (mc2_activeMotor == 1) {
        digitalWrite(MC2_MOTOR_A, HIGH);
        digitalWrite(MC2_MOTOR_B, LOW);
    } else {
        digitalWrite(MC2_MOTOR_A, LOW);
        digitalWrite(MC2_MOTOR_B, HIGH);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// motorControl3
// ──────────────────────────────────────────────────────────────────────────────
void motorControl3() {
    uint32_t now = millis();

    if (now - mc3_lastSwitch >= MOTOR_SWITCH_INTERVAL) {
        mc3_lastSwitch  = now;
        mc3_activeMotor = (mc3_activeMotor == 1) ? 2 : 1;
        Serial.print("[MC3] Switched to Motor ");
        Serial.println(mc3_activeMotor == 1 ? "A" : "B");
    }

    if (mc3_activeMotor == 1) {
        digitalWrite(MC3_MOTOR_A, HIGH);
        digitalWrite(MC3_MOTOR_B, LOW);
    } else {
        digitalWrite(MC3_MOTOR_A, LOW);
        digitalWrite(MC3_MOTOR_B, HIGH);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    pinMode(MC2_MOTOR_A, OUTPUT);  pinMode(MC2_MOTOR_B, OUTPUT);
    pinMode(MC3_MOTOR_A, OUTPUT);  pinMode(MC3_MOTOR_B, OUTPUT);

    // Start with all motors OFF
    digitalWrite(MC2_MOTOR_A, LOW);  digitalWrite(MC2_MOTOR_B, LOW);
    digitalWrite(MC3_MOTOR_A, LOW);  digitalWrite(MC3_MOTOR_B, LOW);

    // Prime loop timer so first execution fires immediately
    lastLoopRun = millis() - LOOP_INTERVAL;

    initEspNow();

    Serial.println("Motor controller started. Motor A active in each group.");
}

// ──────────────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // Non-blocking 5-second gate
    if (now - lastLoopRun >= LOOP_INTERVAL) {
        lastLoopRun = now;

        motorControl2();
        motorControl3();
        sendMotorStatus();

        if (receiverAckedLastMessage) {
            Serial.print("Last ACK age(ms): ");
            Serial.println(now - lastAckReceivedAt);
        } else {
            Serial.println("Waiting for receiver ACK");
        }
    }
}
