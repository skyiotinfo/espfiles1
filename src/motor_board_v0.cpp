// ═══════════════════════════════════════════════════════════════════════════
//  MOTOR BOARD — Production Ready v3.0
//
//  Architecture:
//    - Motor Board controls the pump relay (D8)
//    - ESP-NOW chain: Motor → Valve1 → Valve2 → Valve3
//    - Serial (9600 baud) ↔ Internet Board (TX→RX, RX←TX, GND-GND)
//    - RTC DS1307 for schedule timing (I2C: SDA/SCL)
//    - Manual push button on D9 (INPUT_PULLUP, LOW when pressed)
//
//  Signal encoding (uint16_t):
//    Hundreds digit = V1 command (1 = open)
//    Tens digit     = V2 command (1 = open)
//    Units digit    = V3 command (1 = open)
//    e.g. 100 = V1 only, 010 = V2 only, 001 = V3 only, 111 = all
//
//  ACK encoding (uint16_t):
//    200 = V1 confirmed open
//    020 = V2 confirmed open
//    002 = V3 confirmed open
//    (additive, e.g. 222 = all open)
//
//  Serial protocol → Internet Board (every 2s):
//    STATUS:motor=1,v1=1,v2=0,v3=0,signal=100,ack=200,mode=SCHED\n
//
//  Serial protocol ← Internet Board:
//    SCH:v1_start=08:00,v1_dur=10,v2_start=08:15,v2_dur=10,v3_start=08:30,v3_dur=10\n
//    CMD:manual_start\n   → open V1→V2→V3 sequentially (2 min each)
//    CMD:manual_stop\n    → stop everything
//    CMD:all_off\n        → stop motor + close all valves
//    CMD:v1_on\n          → open V1 only, start motor
//    CMD:v2_on\n          → open V2 only, start motor
//    CMD:v3_on\n          → open V3 only, start motor
//
//  Manual button behavior:
//    First press  → V1 open (2 min) → V2 open (2 min) → V3 open (2 min) → stop
//    Second press → stop immediately
//
//  Test cases covered:
//    [TC-01] Normal schedule start/stop
//    [TC-02] Manual button V1→V2→V3 sequence
//    [TC-03] Manual stop mid-sequence
//    [TC-04] ACK retry and retry failure
//    [TC-05] Late ACK after retry failure (ignored)
//    [TC-06] Heartbeat valve mismatch recovery
//    [TC-07] Force-OFF stuck valve
//    [TC-08] Valve dropped mid-run → motor stops, resend
//    [TC-09] Internet board CMD:manual_start
//    [TC-10] Internet board CMD:v1_on / v2_on / v3_on
//    [TC-11] Schedule cancelled (cancelledThisSlot)
//    [TC-12] EEPROM save/load on power cycle
//    [TC-13] RTC unavailable → schedules disabled
//    [TC-14] V2/V3 offline → motor runs on V1 only
//    [TC-15] Status sent every 2s to internet board
// ═══════════════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include "RTClib.h"
#include <Wire.h>
#include <EEPROM.h>
extern "C" {
  #include "user_interface.h"
}

// ─── Pins ────────────────────────────────────────────────────────────────────
#define MOTOR_PIN   D8
#define BUTTON_PIN  D9

// ─── EEPROM layout ───────────────────────────────────────────────────────────
#define EEPROM_SIZE   32
#define EE_V1_H        0    // uint8  – V1 start hour
#define EE_V1_M        1    // uint8  – V1 start minute
#define EE_V1_DUR      2    // uint16 – V1 duration seconds
#define EE_V2_H        4
#define EE_V2_M        5
#define EE_V2_DUR      6
#define EE_V3_H        8
#define EE_V3_M        9
#define EE_V3_DUR     10
#define EE_VALID      12    // 0xAB = data is valid

// ─── ESP-NOW peer (Valve 1) ────────────────────────────────────────────────
// *** UPDATE this to match Valve 1's actual MAC (printed on boot) ***
uint8_t valve1Mac[] = {0xC8, 0xC9, 0xA3, 0x39, 0xA4, 0xDE};

// ─── RTC ─────────────────────────────────────────────────────────────────────
RTC_DS1307 rtc;
bool rtcAvailable = false;

// ─── Schedule structure ───────────────────────────────────────────────────────
struct Schedule {
  uint16_t valveMask;       // 100 / 10 / 1
  uint8_t  startHour;
  uint8_t  startMin;
  uint16_t durationSec;     // 0 = not configured
  bool     active;
  bool     cancelledThisSlot;
};

Schedule schedules[3] = {
  {100, 0, 0, 0, false, false},   // V1
  { 10, 0, 0, 0, false, false},   // V2
  {  1, 0, 0, 0, false, false},   // V3
};

// ─── Timing constants ─────────────────────────────────────────────────────────
const unsigned long HB_MOTOR_ON_INTERVAL  = 10000UL;   // 10 s heartbeat when motor running
const unsigned long HB_MOTOR_OFF_INTERVAL = 60000UL;   // 60 s heartbeat when motor idle
const unsigned long ACK_TIMEOUT_MS        = 3000UL;
const unsigned long SWITCH_TIMEOUT_MS     = 15000UL;   // max wait for valve close ack
const unsigned long MANUAL_DURATION       = 120000UL;  // 2 min per valve in manual mode
const unsigned long STATUS_INTERVAL       = 2000UL;    // send status to internet board
const uint8_t       MAX_RETRIES           = 5;
const uint8_t       MAX_FORCE_OFF_RETRIES = 10;

// ─── State ───────────────────────────────────────────────────────────────────
bool     motorRunning            = false;
bool     motorStoppedByRetryFail = false;
uint16_t currentSignal           = 0;
uint16_t lastAck                 = 9999;
uint16_t lastRunningAck          = 0;

bool          waitingForAck  = false;
unsigned long ackSentAt      = 0;
uint16_t      lastSentSignal = 0;
uint8_t       retryCount     = 0;

unsigned long lastHeartbeatAt  = 0;
bool          heartbeatPending = false;

bool          forceOffActive  = false;
uint8_t       forceOffRetries = 0;
unsigned long forceOffSentAt  = 0;

unsigned long scheduleStopAt[3] = {0, 0, 0};
unsigned long lastStatusSent    = 0;
unsigned long lastButtonPress   = 0;

// ─── ACK queue (ISR-safe) ─────────────────────────────────────────────────────
volatile uint16_t ackQueue[16];
volatile uint8_t  ackQueueIdx     = 0;
volatile bool     newAckAvailable = false;

// ─── Manual sequence FSM ──────────────────────────────────────────────────────
enum ManualState {
  MANUAL_IDLE, MANUAL_V1, MANUAL_SWITCH_V2,
  MANUAL_V2,   MANUAL_SWITCH_V3, MANUAL_V3, MANUAL_DONE
};
ManualState   manualState      = MANUAL_IDLE;
unsigned long manualStepStart  = 0;
bool          switchPhase2Sent = false;

// ─── Serial buffer (commands from internet board) ────────────────────────────
String serialBuffer = "";

// ─── Forward declarations ─────────────────────────────────────────────────────
void     sendSignal(uint16_t);
void     startMotor();
void     stopMotorOnly();
void     stopAll();
void     cancelAllSchedules();
uint16_t requestedAckBits(uint16_t);
bool     ackIsAcceptable(uint16_t, uint16_t);
bool     valveDropped(uint16_t, uint16_t);
void     triggerForceOff(const char*);
void     handleForceOff();
void     handleHeartbeat();
void     checkSchedules();
void     handleManualButton();
void     handleQueuedAcks();
void     runManualSequence();
void     retryIfNoAck();
void     processAck(uint16_t);
void     sendStatusToInternet();
void     readSerialFromInternet();
void     handleSerialCommand(const String&);
void     parseScheduleCommand(const String&);
void     saveSchedulesToEEPROM();
void     loadSchedulesFromEEPROM();
void     printRtcTime();

// ═══════════════════════════════════════════════════════════════════════════
//  EEPROM
// ═══════════════════════════════════════════════════════════════════════════

void saveSchedulesToEEPROM() {
  EEPROM.write(EE_V1_H,  schedules[0].startHour);
  EEPROM.write(EE_V1_M,  schedules[0].startMin);
  EEPROM.put (EE_V1_DUR, schedules[0].durationSec);
  EEPROM.write(EE_V2_H,  schedules[1].startHour);
  EEPROM.write(EE_V2_M,  schedules[1].startMin);
  EEPROM.put (EE_V2_DUR, schedules[1].durationSec);
  EEPROM.write(EE_V3_H,  schedules[2].startHour);
  EEPROM.write(EE_V3_M,  schedules[2].startMin);
  EEPROM.put (EE_V3_DUR, schedules[2].durationSec);
  EEPROM.write(EE_VALID, 0xAB);
  EEPROM.commit();
  Serial.println(F("[EEPROM] Schedules saved"));
}

void loadSchedulesFromEEPROM() {
  if (EEPROM.read(EE_VALID) != 0xAB) {
    Serial.println(F("[EEPROM] No valid data"));
    return;
  }
  uint8_t h, m; uint16_t d;

  h = EEPROM.read(EE_V1_H); m = EEPROM.read(EE_V1_M); EEPROM.get(EE_V1_DUR, d);
  if (h < 24 && m < 60) { schedules[0].startHour = h; schedules[0].startMin = m; schedules[0].durationSec = d; }

  h = EEPROM.read(EE_V2_H); m = EEPROM.read(EE_V2_M); EEPROM.get(EE_V2_DUR, d);
  if (h < 24 && m < 60) { schedules[1].startHour = h; schedules[1].startMin = m; schedules[1].durationSec = d; }

  h = EEPROM.read(EE_V3_H); m = EEPROM.read(EE_V3_M); EEPROM.get(EE_V3_DUR, d);
  if (h < 24 && m < 60) { schedules[2].startHour = h; schedules[2].startMin = m; schedules[2].durationSec = d; }

  Serial.printf("[EEPROM] Loaded: V1=%02d:%02d/%ds V2=%02d:%02d/%ds V3=%02d:%02d/%ds\n",
    schedules[0].startHour, schedules[0].startMin, schedules[0].durationSec,
    schedules[1].startHour, schedules[1].startMin, schedules[1].durationSec,
    schedules[2].startHour, schedules[2].startMin, schedules[2].durationSec);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ACK helpers
// ═══════════════════════════════════════════════════════════════════════════

uint16_t requestedAckBits(uint16_t signal) {
  return ((signal / 100) % 10 ? 200 : 0)
       + ((signal / 10)  % 10 ? 20  : 0)
       + ( signal        % 10 ? 2   : 0);
}

bool ackIsAcceptable(uint16_t signal, uint16_t ack) {
  if (signal == 0) return (ack == 0);
  uint16_t req = requestedAckBits(signal);
  return (ack & req) != 0 && (ack & ~req) == 0;
}

// Returns true if any bit that was set in prev is now cleared in now
bool valveDropped(uint16_t prev, uint16_t now) {
  return (prev & ~now) != 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Signal send
// ═══════════════════════════════════════════════════════════════════════════

void sendSignal(uint16_t signal) {
  motorStoppedByRetryFail = false;
  lastSentSignal = signal;
  waitingForAck  = true;
  ackSentAt      = millis();
  retryCount     = 0;
  esp_now_send(valve1Mac, (uint8_t*)&signal, sizeof(signal));
  Serial.printf("[TX] Signal → V1: %03d\n", signal);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Motor control
// ═══════════════════════════════════════════════════════════════════════════

void startMotor() {
  if (motorRunning) return;
  delay(200);
  digitalWrite(MOTOR_PIN, HIGH);
  motorRunning    = true;
  lastRunningAck  = lastAck;
  lastHeartbeatAt = millis();
  Serial.println(F("[MOTOR] ON ← valves confirmed open"));
}

void stopMotorOnly() {
  if (!motorRunning) return;
  digitalWrite(MOTOR_PIN, LOW);
  motorRunning    = false;
  lastRunningAck  = 0;
  lastHeartbeatAt = millis();
  Serial.println(F("[MOTOR] OFF"));
}

void stopAll() {
  if (motorRunning) {
    digitalWrite(MOTOR_PIN, LOW);
    motorRunning    = false;
    lastRunningAck  = 0;
    lastHeartbeatAt = millis();
    Serial.println(F("[MOTOR] OFF (stopAll)"));
    delay(500);
  }
  currentSignal = 0;
  sendSignal(0);
}

void cancelAllSchedules() {
  for (int i = 0; i < 3; i++) {
    if (schedules[i].active) {
      schedules[i].active            = false;
      schedules[i].cancelledThisSlot = true;
      Serial.printf("[SCH] Schedule %d cancelled\n", i);
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Force OFF  (persistently closes all valves)
// ═══════════════════════════════════════════════════════════════════════════

void triggerForceOff(const char* reason) {
  if (forceOffActive) return;
  Serial.printf("[FORCE-OFF] Triggered: %s\n", reason);
  forceOffActive  = true;
  forceOffRetries = 0;
  forceOffSentAt  = 0;
}

void handleForceOff() {
  if (!forceOffActive) return;
  if (lastAck == 0) {
    Serial.println(F("[FORCE-OFF] Done — all valves closed"));
    forceOffActive  = false;
    forceOffRetries = 0;
    return;
  }
  if (forceOffRetries >= MAX_FORCE_OFF_RETRIES) {
    Serial.println(F("[FORCE-OFF] FAILED — valve may be physically stuck!"));
    forceOffActive = false;
    return;
  }
  if (millis() - forceOffSentAt >= 2000) {
    forceOffSentAt = millis();
    forceOffRetries++;
    uint16_t off = 0;
    esp_now_send(valve1Mac, (uint8_t*)&off, sizeof(off));
    Serial.printf("[FORCE-OFF] Attempt %d/%d\n", forceOffRetries, MAX_FORCE_OFF_RETRIES);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Heartbeat  (periodic valve state verification)
// ═══════════════════════════════════════════════════════════════════════════

void handleHeartbeat() {
  if (forceOffActive || waitingForAck) return;
  unsigned long interval = motorRunning ? HB_MOTOR_ON_INTERVAL : HB_MOTOR_OFF_INTERVAL;
  if (millis() - lastHeartbeatAt < interval) return;
  lastHeartbeatAt  = millis();
  heartbeatPending = true;
  sendSignal(motorRunning ? currentSignal : 0);
  Serial.printf("[HB] Sending heartbeat — signal=%03d motor=%s\n",
    motorRunning ? currentSignal : 0, motorRunning ? "ON" : "OFF");
}

// ═══════════════════════════════════════════════════════════════════════════
//  Retry (when no ACK received within ACK_TIMEOUT_MS)
// ═══════════════════════════════════════════════════════════════════════════

void retryIfNoAck() {
  if (!waitingForAck) return;
  if (millis() - ackSentAt < ACK_TIMEOUT_MS) return;
  if (retryCount >= MAX_RETRIES) {
    Serial.printf("[RETRY] FAILED after %d retries for signal %03d\n", MAX_RETRIES, lastSentSignal);
    waitingForAck    = false;
    heartbeatPending = false;
    if (motorRunning) {
      stopMotorOnly();
      motorStoppedByRetryFail = true;
    }
    return;
  }
  retryCount++;
  ackSentAt = millis();
  esp_now_send(valve1Mac, (uint8_t*)&lastSentSignal, sizeof(lastSentSignal));
  Serial.printf("[RETRY] %d/%d for signal %03d\n", retryCount, MAX_RETRIES, lastSentSignal);
}

// ═══════════════════════════════════════════════════════════════════════════
//  ACK processing (core state machine)
// ═══════════════════════════════════════════════════════════════════════════

void processAck(uint16_t ack) {
  lastAck       = ack;
  waitingForAck = false;
  Serial.printf("[ACK] Received: %03d (signal=%03d motor=%s)\n",
    ack, currentSignal, motorRunning ? "ON" : "OFF");

  // ── Force-OFF path ─────────────────────────────────────────────────────
  if (forceOffActive) {
    if (ack == 0) { forceOffActive = false; forceOffRetries = 0; }
    return;
  }

  // ── Heartbeat path ─────────────────────────────────────────────────────
  if (heartbeatPending) {
    heartbeatPending = false;
    if (motorRunning) {
      if (!ackIsAcceptable(currentSignal, ack)) {
        Serial.printf("[HB] Mismatch! expected≈%03d got %03d → resync\n",
          requestedAckBits(currentSignal), ack);
        stopMotorOnly();
        delay(500);
        sendSignal(currentSignal);
      } else {
        lastRunningAck = ack;
        Serial.printf("[HB] OK — ack=%03d\n", ack);
      }
    } else {
      if (ack != 0) triggerForceOff("valve ON during idle heartbeat");
      else          Serial.println(F("[HB] Safety OK — all valves off"));
    }
    return;
  }

  // ── Mid-run valve-drop detection ───────────────────────────────────────
  if (motorRunning && currentSignal != 0) {
    if (ack == 0) { stopMotorOnly(); return; }
    if (valveDropped(lastRunningAck, ack)) {
      Serial.println(F("[ACK] Valve dropped mid-run → motor stop → resend"));
      stopMotorOnly();
      delay(300);
      sendSignal(currentSignal);
      return;
    }
    if (ack != lastRunningAck) lastRunningAck = ack;
    return;
  }

  // ── Normal open/close path ─────────────────────────────────────────────
  if (currentSignal != 0) {
    if (ackIsAcceptable(currentSignal, ack)) {
      if (!motorRunning) {
        if (motorStoppedByRetryFail) {
          Serial.printf("[ACK] Late ACK %03d after retry failure — ignored\n", ack);
          motorStoppedByRetryFail = false;
          return;
        }
        Serial.printf("[ACK] Valves confirmed %03d → starting motor\n", ack);
        startMotor();
      }
    } else if (ack == 0 && motorRunning) {
      Serial.println(F("[ACK] All valves closed unexpectedly → stopping motor"));
      stopMotorOnly();
    } else {
      Serial.printf("[ACK] Mismatch sent=%03d got=%03d → retry\n", currentSignal, ack);
      sendSignal(currentSignal);
    }
  } else {
    if (ack != 0) {
      Serial.printf("[ACK] Valves still open (%03d) after close → retry\n", ack);
      sendSignal(0);
    } else {
      Serial.println(F("[ACK] All valves confirmed closed"));
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  ACK queue handler (dequeues ISR-written ACKs safely)
// ═══════════════════════════════════════════════════════════════════════════

void handleQueuedAcks() {
  if (!newAckAvailable) return;
  noInterrupts();
  uint8_t count = ackQueueIdx;
  ackQueueIdx   = 0;
  interrupts();
  for (uint8_t i = 0; i < count; i++) processAck(ackQueue[i]);
  newAckAvailable = false;
}

// ═══════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═══════════════════════════════════════════════════════════════════════════

void onReceive(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (len < 2 || ackQueueIdx >= 16) return;
  uint16_t ack = 0;
  memcpy(&ack, data, sizeof(uint16_t));
  ackQueue[ackQueueIdx++] = ack;
  newAckAvailable         = true;
}

void onSent(uint8_t* mac, uint8_t status) {}

// ═══════════════════════════════════════════════════════════════════════════
//  Schedule handler (RTC-based)
// ═══════════════════════════════════════════════════════════════════════════

void checkSchedules() {
  if (!rtcAvailable || manualState != MANUAL_IDLE) return;

  DateTime now    = rtc.now();
  uint32_t nowSec = (uint32_t)now.hour() * 3600UL
                  + (uint32_t)now.minute() * 60UL
                  + now.second();
  uint16_t combined = 0;

  for (int i = 0; i < 3; i++) {
    if (schedules[i].durationSec == 0) continue;

    uint32_t startSec = (uint32_t)schedules[i].startHour * 3600UL
                      + (uint32_t)schedules[i].startMin  * 60UL;
    uint32_t stopSec  = startSec + schedules[i].durationSec;
    bool     inWin    = (nowSec >= startSec && nowSec < stopSec);

    // Reset cancellation flag once window has passed
    if (schedules[i].cancelledThisSlot && !inWin)
      schedules[i].cancelledThisSlot = false;

    // Activate
    if (!schedules[i].active && !schedules[i].cancelledThisSlot && inWin) {
      schedules[i].active   = true;
      scheduleStopAt[i]     = millis() + (uint32_t)(stopSec - nowSec) * 1000UL;
      Serial.printf("[SCH] Schedule %d START (valve=%03d, %lus remaining)\n",
        i, schedules[i].valveMask, (unsigned long)(stopSec - nowSec));
    }
    // Deactivate
    if (schedules[i].active && millis() >= scheduleStopAt[i]) {
      schedules[i].active = false;
      Serial.printf("[SCH] Schedule %d STOP\n", i);
    }
    if (schedules[i].active) combined |= schedules[i].valveMask;
  }

  if (combined != currentSignal) {
    currentSignal = combined;
    if (currentSignal == 0) stopAll();
    else sendSignal(currentSignal);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Manual button handler
// ═══════════════════════════════════════════════════════════════════════════

void handleManualButton() {
  if (digitalRead(BUTTON_PIN) != LOW) return;
  if (millis() - lastButtonPress <= 500) return;
  lastButtonPress = millis();

  if (manualState == MANUAL_IDLE) {
    Serial.println(F("[BTN] Manual start → V1→V2→V3 sequence"));
    cancelAllSchedules();
    if (motorRunning) { stopMotorOnly(); delay(500); }
    manualState      = MANUAL_V1;
    manualStepStart  = millis();
    currentSignal    = 100;
    switchPhase2Sent = false;
    sendSignal(100);
  } else {
    Serial.println(F("[BTN] Manual stop"));
    currentSignal = 0;
    manualState   = MANUAL_IDLE;
    stopAll();
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Manual sequence runner (FSM)
// ═══════════════════════════════════════════════════════════════════════════

void runManualSequence() {
  if (manualState == MANUAL_IDLE) return;
  if (manualState == MANUAL_DONE) { manualState = MANUAL_IDLE; Serial.println(F("[MANUAL] Complete")); return; }
  if (waitingForAck || forceOffActive) return;

  unsigned long el = millis() - manualStepStart;

  switch (manualState) {

    case MANUAL_V1:
      if (el >= MANUAL_DURATION) {
        Serial.println(F("[MANUAL] V1 done → stopping motor, closing V1"));
        stopMotorOnly(); delay(2000);
        currentSignal    = 0;
        switchPhase2Sent = false;
        manualStepStart  = millis();
        manualState      = MANUAL_SWITCH_V2;
        sendSignal(0);
      }
      break;

    case MANUAL_SWITCH_V2:
      if (!switchPhase2Sent &&
          (lastAck == 0 || millis() - manualStepStart > SWITCH_TIMEOUT_MS)) {
        if (lastAck != 0) Serial.println(F("[MANUAL] V1 close not confirmed — continuing"));
        switchPhase2Sent = true;
        manualStepStart  = millis();
        manualState      = MANUAL_V2;
        currentSignal    = 10;
        Serial.println(F("[MANUAL] Opening V2"));
        sendSignal(10);
      }
      break;

    case MANUAL_V2:
      if (el >= MANUAL_DURATION) {
        Serial.println(F("[MANUAL] V2 done → stopping motor, closing V2"));
        stopMotorOnly(); delay(2000);
        currentSignal    = 0;
        switchPhase2Sent = false;
        manualStepStart  = millis();
        manualState      = MANUAL_SWITCH_V3;
        sendSignal(0);
      }
      break;

    case MANUAL_SWITCH_V3:
      if (!switchPhase2Sent &&
          (lastAck == 0 || millis() - manualStepStart > SWITCH_TIMEOUT_MS)) {
        if (lastAck != 0) Serial.println(F("[MANUAL] V2 close not confirmed — continuing"));
        switchPhase2Sent = true;
        manualStepStart  = millis();
        manualState      = MANUAL_V3;
        currentSignal    = 1;
        Serial.println(F("[MANUAL] Opening V3"));
        sendSignal(1);
      }
      break;

    case MANUAL_V3:
      if (el >= MANUAL_DURATION) {
        Serial.println(F("[MANUAL] V3 done → stopping all"));
        currentSignal = 0;
        manualState   = MANUAL_DONE;
        stopAll();
      }
      break;

    default: break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Serial communication with Internet Board
// ═══════════════════════════════════════════════════════════════════════════

void sendStatusToInternet() {
  if (millis() - lastStatusSent < STATUS_INTERVAL) return;
  lastStatusSent = millis();

  uint8_t v1 = (lastAck >= 200) ? 1 : 0;
  uint8_t v2 = ((lastAck % 200) >= 20) ? 1 : 0;
  uint8_t v3 = ((lastAck % 20)  >= 2)  ? 1 : 0;

  Serial.printf("STATUS:motor=%d,v1=%d,v2=%d,v3=%d,signal=%03d,ack=%03d,mode=%s\n",
    motorRunning ? 1 : 0, v1, v2, v3,
    currentSignal, lastAck,
    manualState != MANUAL_IDLE ? "MANUAL" : "SCHED");
}

void parseScheduleCommand(const String& line) {
  auto extractStr = [&](const String& key) -> String {
    int idx = line.indexOf(key + "=");
    if (idx < 0) return "";
    int s = idx + key.length() + 1;
    int e = line.indexOf(',', s);
    return line.substring(s, e < 0 ? line.length() : e);
  };
  auto extractInt = [&](const String& key) -> int {
    String v = extractStr(key);
    return v.length() ? v.toInt() : -1;
  };

  bool changed = false;
  for (int i = 0; i < 3; i++) {
    String p = "v" + String(i + 1);
    String t = extractStr(p + "_start");
    int    d = extractInt(p + "_dur");     // minutes
    if (t.length() >= 5 && d >= 0) {
      uint8_t  h = t.substring(0, 2).toInt();
      uint8_t  m = t.substring(3, 5).toInt();
      uint16_t s = (uint16_t)(d * 60);
      if (h < 24 && m < 60) {
        schedules[i].startHour       = h;
        schedules[i].startMin        = m;
        schedules[i].durationSec     = s;
        schedules[i].active          = false;
        schedules[i].cancelledThisSlot = false;
        changed = true;
        Serial.printf("[SCH] V%d schedule: %02d:%02d for %ds\n", i+1, h, m, s);
      }
    }
  }
  if (changed) saveSchedulesToEEPROM();
}

void handleSerialCommand(const String& line) {
  Serial.printf("[CMD] Received: %s\n", line.c_str());

  if (line.startsWith("SCH:")) {
    parseScheduleCommand(line);

  } else if (line == "CMD:manual_start") {
    // App-triggered manual → V1→V2→V3 2 min each
    if (manualState == MANUAL_IDLE) {
      cancelAllSchedules();
      if (motorRunning) { stopMotorOnly(); delay(500); }
      manualState      = MANUAL_V1;
      manualStepStart  = millis();
      currentSignal    = 100;
      switchPhase2Sent = false;
      sendSignal(100);
    }

  } else if (line == "CMD:manual_stop" || line == "CMD:all_off") {
    cancelAllSchedules();
    currentSignal = 0;
    manualState   = MANUAL_IDLE;
    stopAll();

  } else if (line == "CMD:v1_on") {
    cancelAllSchedules();
    if (motorRunning) { stopMotorOnly(); delay(500); }
    currentSignal = 100;
    sendSignal(100);

  } else if (line == "CMD:v2_on") {
    cancelAllSchedules();
    if (motorRunning) { stopMotorOnly(); delay(500); }
    currentSignal = 10;
    sendSignal(10);

  } else if (line == "CMD:v3_on") {
    cancelAllSchedules();
    if (motorRunning) { stopMotorOnly(); delay(500); }
    currentSignal = 1;
    sendSignal(1);
  }
}

void readSerialFromInternet() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.startsWith("SCH:") || serialBuffer.startsWith("CMD:"))
        handleSerialCommand(serialBuffer);
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
      if (serialBuffer.length() > 200) serialBuffer = "";
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  RTC time display (for verification)
// ═══════════════════════════════════════════════════════════════════════════

void printRtcTime() {
  static unsigned long last = 0;
  if (millis() - last < 10000) return;
  last = millis();

  if (rtcAvailable) {
    DateTime now = rtc.now();
    Serial.printf("\n[RTC] Time: %02d:%02d:%02d  Motor:%s  Signal:%03d  ACK:%03d  Mode:%s\n",
      now.hour(), now.minute(), now.second(),
      motorRunning ? "ON" : "OFF",
      currentSignal, lastAck,
      manualState != MANUAL_IDLE ? "MANUAL" : "SCHED");

    for (int i = 0; i < 3; i++) {
      if (schedules[i].durationSec > 0) {
        if (schedules[i].active) {
          long rem = (long)(scheduleStopAt[i] - millis()) / 1000L;
          Serial.printf("[SCH]   V%d: RUNNING %lds left\n", i+1, max(rem, 0L));
        } else {
          Serial.printf("[SCH]   V%d: idle  start=%02d:%02d dur=%ds cancelled=%d\n",
            i+1, schedules[i].startHour, schedules[i].startMin,
            schedules[i].durationSec, schedules[i].cancelledThisSlot);
        }
      }
    }

    if (manualState != MANUAL_IDLE && manualState != MANUAL_DONE) {
      long rem = (long)(MANUAL_DURATION - (millis() - manualStepStart)) / 1000L;
      const char* steps[] = {"","V1","→V2","V2","→V3","V3","Done"};
      Serial.printf("[MANUAL] step=%s %lds left\n", steps[(int)manualState], max(rem, 0L));
    }
  } else {
    Serial.println(F("[RTC] Not available — schedules disabled"));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  // MUST match internet board SoftwareSerial baud
  Serial.begin(9600);

  EEPROM.begin(EEPROM_SIZE);

  pinMode(MOTOR_PIN,  OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin();
  if (!rtc.begin()) {
    Serial.println(F("[RTC] Not found — schedules disabled"));
    rtcAvailable = false;
  } else {
    if (!rtc.isrunning()) {
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println(F("[RTC] Set to compile time"));
    }
    rtcAvailable = true;
    DateTime now = rtc.now();
    Serial.printf("[RTC] Time: %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
  }

  loadSchedulesFromEEPROM();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(1);

  Serial.print(F("[ESPNOW] Motor MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() == 0) {
    esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
    esp_now_register_send_cb(onSent);
    esp_now_register_recv_cb(onReceive);
    esp_now_add_peer(valve1Mac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
    Serial.println(F("[ESPNOW] Init OK"));
  } else {
    Serial.println(F("[ESPNOW] Init FAILED"));
  }

  lastHeartbeatAt = millis();
  Serial.println(F("[BOOT] Motor Board ready\n"));
}

// ═══════════════════════════════════════════════════════════════════════════
//  Loop
// ═══════════════════════════════════════════════════════════════════════════

void loop() {
  readSerialFromInternet();
  handleQueuedAcks();
  handleManualButton();
  runManualSequence();
  checkSchedules();
  retryIfNoAck();
  handleForceOff();
  handleHeartbeat();
  sendStatusToInternet();
  printRtcTime();
  delay(50);
}