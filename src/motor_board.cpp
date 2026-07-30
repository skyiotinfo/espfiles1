#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include "RTClib.h"
#include <Wire.h>
extern "C"
{
#include "user_interface.h"
}

#define MOTOR_PIN D8
#define BUTTON_PIN D9

uint8_t valve1Mac[] = {0xFC, 0xF5, 0xC4, 0xBE, 0xD2, 0xDC};
// uint8_t valve1Mac[] = {0xD8, 0xBF, 0xC0, 0x06, 0xDE, 0xC0};

RTC_DS1307 rtc;
bool rtcAvailable = false;

struct Schedule
{
  uint16_t valveMask;
  uint8_t startHour;
  uint8_t startMin;
  uint16_t durationSec;
  bool active;
};

Schedule schedules[3] = {
    {100, 20, 44, 100, false},
    {10, 20, 46, 100, false},
    {1, 20, 48, 40, false},
};
//
const unsigned long HB_MOTOR_ON_INTERVAL = 10000UL;
const unsigned long HB_MOTOR_OFF_INTERVAL = 600000UL;
const unsigned long ACK_TIMEOUT_MS = 3000UL;
const unsigned long SWITCH_TIMEOUT_MS = 15000UL; // FIX 3: max wait in SWITCH states
const uint8_t MAX_RETRIES = 5;
const uint8_t MAX_FORCE_OFF_RETRIES = 10;

bool motorRunning = false;
bool motorStoppedByRetryFail = false; // FIX 2: blocks late ACK from restarting motor
uint16_t currentSignal = 0;
uint16_t lastAck = 9999;

bool waitingForAck = false;
unsigned long ackSentAt = 0;
uint16_t lastSentSignal = 0;
uint8_t retryCount = 0;

unsigned long lastHeartbeatAt = 0;
bool heartbeatPending = false;

bool forceOffActive = false;
uint8_t forceOffRetries = 0;
unsigned long forceOffSentAt = 0;

unsigned long scheduleStopAt[3] = {0, 0, 0};

enum ManualState
{
  MANUAL_IDLE,
  MANUAL_V1,
  MANUAL_SWITCH_V2,
  MANUAL_V2,
  MANUAL_SWITCH_V3,
  MANUAL_V3,
  MANUAL_DONE
};
ManualState manualState = MANUAL_IDLE;
unsigned long manualStepStart = 0;
const unsigned long MANUAL_DURATION = 40000UL;

bool switchPhase2Sent = false;
unsigned long lastButtonPress = 0;

volatile uint16_t ackQueue[16];
volatile uint8_t ackQueueIdx = 0;
volatile bool newAckAvailable = false;

// ─── Forward declarations ────────────────────────────────────────────────────
void sendSignal(uint16_t signal);
void startMotor();
void stopMotorOnly();
void stopAll();
uint16_t requestedAckBits(uint16_t signal);
bool ackIsAcceptable(uint16_t signal, uint16_t ack);
void triggerForceOff(const char *reason);
void handleForceOff();
void handleHeartbeat();
void checkSchedules();
void handleManualButton();
void handleQueuedAcks();
void runManualSequence();
void retryIfNoAck();
void processAck(uint16_t ack);
void printStatus();

// ─── ACK helpers ─────────────────────────────────────────────────────────────

uint16_t requestedAckBits(uint16_t signal)
{
  uint16_t v1 = (signal / 100) % 10;
  uint16_t v2 = (signal / 10) % 10;
  uint16_t v3 = signal % 10;
  return (v1 ? 200 : 0) + (v2 ? 20 : 0) + (v3 ? 2 : 0);
}

bool ackIsAcceptable(uint16_t signal, uint16_t ack)
{
  if (signal == 0)
    return (ack == 0);
  uint16_t requested = requestedAckBits(signal);
  bool anyConfirmed = (ack & requested) != 0;
  bool noRogues = (ack & ~requested) == 0;
  return anyConfirmed && noRogues;
}

// ─── Signal send ─────────────────────────────────────────────────────────────

void sendSignal(uint16_t signal)
{
  motorStoppedByRetryFail = false; // FIX 2: new intentional send clears the flag
  lastSentSignal = signal;
  waitingForAck = true;
  ackSentAt = millis();
  retryCount = 0;
  esp_now_send(valve1Mac, (uint8_t *)&signal, sizeof(signal));
  Serial.printf("📤 STEP 1: Sent valve signal: %03d\n", signal);
}

// ─── Motor control ───────────────────────────────────────────────────────────

void startMotor()
{
  if (!motorRunning)
  {
    delay(200);
    digitalWrite(MOTOR_PIN, HIGH);
    motorRunning = true;
    lastHeartbeatAt = millis();
    Serial.println("🔌 STEP 3: Motor started <- valves are now open and confirmed");
    Serial.println("═══════════════════════════════════════════════════════════");
  }
}

void stopMotorOnly()
{
  if (motorRunning)
  {
    digitalWrite(MOTOR_PIN, LOW);
    motorRunning = false;
    lastHeartbeatAt = millis();
    Serial.println("⏹️  STEP 1: Motor OFF <- pressure relief before valve change");
  }
}

void stopAll()
{
  if (motorRunning)
  {
    digitalWrite(MOTOR_PIN, LOW);
    motorRunning = false;
    lastHeartbeatAt = millis();
    Serial.println("⏹️  STEP 1: Motor OFF <- stopping pump first");
    delay(500);
    Serial.println("⏳ STEP 2: Waiting 500ms for motor inertia to stop...");
  }
  currentSignal = 0;
  sendSignal(0);
  Serial.println("📤 STEP 3: Sent close signal (000) to shut all valves");
  Serial.println("═══════════════════════════════════════════════════════════");
}

// ─── Force OFF ───────────────────────────────────────────────────────────────

void triggerForceOff(const char *reason)
{
  if (forceOffActive)
    return;
  Serial.printf("⚠️  FORCE OFF triggered: %s\n", reason);
  forceOffActive = true;
  forceOffRetries = 0;
  forceOffSentAt = 0;
}

void handleForceOff()
{
  if (!forceOffActive)
    return;

  if (lastAck == 0)
  {
    Serial.println("✅ Force OFF confirmed — all valves closed");
    forceOffActive = false;
    forceOffRetries = 0;
    return;
  }

  if (forceOffRetries >= MAX_FORCE_OFF_RETRIES)
  {
    Serial.println("❌ Force OFF failed — valve may be physically stuck!");
    forceOffActive = false;
    return;
  }

  if (millis() - forceOffSentAt >= 2000)
  {
    forceOffSentAt = millis();
    forceOffRetries++;
    uint16_t off = 0;
    esp_now_send(valve1Mac, (uint8_t *)&off, sizeof(off));
    Serial.printf("🔁 Force OFF attempt %d / %d\n", forceOffRetries, MAX_FORCE_OFF_RETRIES);
  }
}

// ─── Heartbeat ───────────────────────────────────────────────────────────────

void handleHeartbeat()
{
  // FIX 1: heartbeat must not fire during the manual sequence
  // if (manualState != MANUAL_IDLE) return;

  if (forceOffActive || waitingForAck)
    return;

  unsigned long interval = motorRunning ? HB_MOTOR_ON_INTERVAL : HB_MOTOR_OFF_INTERVAL;
  // if (millis() - lastHeartbeatAt < interval) return;

  lastHeartbeatAt = millis();
  heartbeatPending = true;

  if (motorRunning)
  {
    Serial.printf("❤️  Heartbeat [Motor ON]  — verifying signal %03d\n", currentSignal);
    sendSignal(currentSignal);
  }
  else
  {
    Serial.println("❤️  Heartbeat [Motor OFF] — checking all valves OFF");
    sendSignal(0);
  }
}

// ─── Retry ───────────────────────────────────────────────────────────────────

void retryIfNoAck()
{
  if (!waitingForAck)
    return;
  if (millis() - ackSentAt < ACK_TIMEOUT_MS)
    return;

  if (retryCount >= MAX_RETRIES)
  {
    Serial.printf("❌ No ACK after %d retries for signal %03d\n", MAX_RETRIES, lastSentSignal);
    waitingForAck = false;
    heartbeatPending = false;

    if (motorRunning)
    {
      Serial.println("⚠️  Valve chain dead — stopping motor for safety");
      stopMotorOnly();
      motorStoppedByRetryFail = true; // FIX 2: flag the failure-stop
    }
    return;
  }

  retryCount++;
  ackSentAt = millis();
  esp_now_send(valve1Mac, (uint8_t *)&lastSentSignal, sizeof(lastSentSignal));
  Serial.printf("🔄 Retry %d / %d — signal %03d\n", retryCount, MAX_RETRIES, lastSentSignal);
}

// ─── ACK processing ──────────────────────────────────────────────────────────

void processAck(uint16_t ack)
{
  lastAck = ack;
  waitingForAck = false;

  // ── Force OFF path ──
  if (forceOffActive)
  {
    if (ack == 0)
    {
      Serial.println("✅ Force OFF confirmed");
      forceOffActive = false;
      forceOffRetries = 0;
    }
    return;
  }

  // ── Heartbeat verification path ──
  if (heartbeatPending)
  {
    heartbeatPending = false;

    if (motorRunning)
    {
      if (!ackIsAcceptable(currentSignal, ack))
      {
        Serial.printf("⚠️  Heartbeat mismatch! Expected ACK ~%03d, got %03d\n",
                      requestedAckBits(currentSignal), ack);
        stopMotorOnly();
        delay(500);
        Serial.println("🔁 Resyncing valves — resending signal");
        sendSignal(currentSignal);
      }
      else
      {
        uint16_t expected = requestedAckBits(currentSignal);
        if (ack != expected)
        {
          Serial.printf("✅ Heartbeat OK (partial) — ACK=%03d, expected=%03d (some valves offline)\n",
                        ack, expected);
        }
        else
        {
          Serial.printf("✅ Heartbeat OK — valves match signal %03d\n", currentSignal);
        }
      }
    }
    else
    {
      if (ack != 0)
      {
        Serial.printf("⚠️  Valve stuck ON (ACK=%03d) while motor OFF\n", ack);
        triggerForceOff("Valve ON during motor-OFF heartbeat");
      }
      else
      {
        Serial.println("✅ Safety check OK — all valves OFF");
      }
    }
    return;
  }

  // ── Normal open/close path ──
  if (currentSignal != 0)
  {

    if (ackIsAcceptable(currentSignal, ack))
    {
      if (!motorRunning)
      {
        // FIX 2: ignore late ACK that arrives after a retry-failure motor stop
        if (motorStoppedByRetryFail)
        {
          Serial.printf("⚠️  Late ACK %03d received after retry failure — ignoring restart\n", ack);
          motorStoppedByRetryFail = false;
          return;
        }

        uint16_t expected = requestedAckBits(currentSignal);
        Serial.printf("📥 STEP 2: Valve ACK received: %03d", ack);
        if (ack != expected)
        {
          Serial.printf("  ⚠️  PARTIAL — some valves offline (expected %03d)\n", expected);
        }
        else
        {
          Serial.println("  (all requested valves OPEN confirmed)");
        }
        Serial.println("🔌 READY TO START MOTOR...");
        startMotor();
      }
    }
    else if (ack == 0 && motorRunning)
    {
      // Ignore temporary 000 during manual sequence
      if (manualState != MANUAL_IDLE &&
          manualState != MANUAL_DONE)
      {
        Serial.println("Manual switching: ignoring temporary ACK=000");
        return;
      }

      Serial.println("⚠️ All valves closed unexpectedly");
      stopMotorOnly();
    }
    else
    {
      Serial.printf("⚠️  ACK mismatch on open: sent %03d, got %03d — retrying\n",
                    currentSignal, ack);
      sendSignal(currentSignal);
    }
  }
  else
  {
    if (ack == 0)
    {
      Serial.println("✅ All valves confirmed closed");
    }
    else
    {
      Serial.printf("⚠️  Valves still open after close (ACK=%03d) — retrying\n", ack);
      sendSignal(0);
    }
  }
}

// ─── Queued ACK handler ──────────────────────────────────────────────────────

void handleQueuedAcks()
{
  if (!newAckAvailable)
    return;

  noInterrupts();
  uint8_t count = ackQueueIdx;
  ackQueueIdx = 0;
  interrupts();

  for (uint8_t i = 0; i < count; i++)
  {
    processAck(ackQueue[i]);
  }

  newAckAvailable = false;
}

// ─── ESP-NOW callbacks ───────────────────────────────────────────────────────

void onReceive(uint8_t *mac, uint8_t *data, uint8_t len)
{
  if (len < 2)
    return;
  if (ackQueueIdx >= 16)
    return;

  uint16_t ack = 0;
  memcpy(&ack, data, sizeof(uint16_t));

  ackQueue[ackQueueIdx++] = ack;
  newAckAvailable = true;

  Serial.printf("📥 ACK received in queue: %03d\n", ack);
}

void onSent(uint8_t *mac, uint8_t status) {}

// ─── Schedule handler ────────────────────────────────────────────────────────

void checkSchedules()
{
  if (!rtcAvailable)
    return;
  if (manualState != MANUAL_IDLE)
    return;

  DateTime now = rtc.now();
  uint32_t nowSec = (uint32_t)now.hour() * 3600UL + (uint32_t)now.minute() * 60UL + now.second();

  uint16_t combinedSignal = 0;

  for (int i = 0; i < 3; i++)
  {
    uint32_t startSec = (uint32_t)schedules[i].startHour * 3600UL + (uint32_t)schedules[i].startMin * 60UL;
    uint32_t stopSec = startSec + schedules[i].durationSec;

    if (!schedules[i].active && nowSec >= startSec && nowSec < stopSec)
    {
      schedules[i].active = true;
      scheduleStopAt[i] = millis() + (uint32_t)(stopSec - nowSec) * 1000UL;
      Serial.printf("⏰ Schedule %d START → valve mask %03d, %lu sec remaining\n",
                    i, schedules[i].valveMask, (unsigned long)(stopSec - nowSec));
    }

    if (schedules[i].active && millis() >= scheduleStopAt[i])
    {
      schedules[i].active = false;
      Serial.printf("⏰ Schedule %d STOP\n", i);
    }

    if (schedules[i].active)
    {
      combinedSignal |= schedules[i].valveMask;
    }
  }

  if (combinedSignal != currentSignal)
  {
    currentSignal = combinedSignal;
    if (currentSignal == 0)
    {
      Serial.println("🚿 All schedules ended — closing all valves");
      stopAll();
    }
    else
    {
      Serial.printf("📡 Combined signal: %03d\n", currentSignal);
      sendSignal(currentSignal);
    }
  }
}

// ─── Manual button ───────────────────────────────────────────────────────────

void handleManualButton()
{
  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastButtonPress > 500)
  {
    lastButtonPress = millis();

    if (manualState == MANUAL_IDLE)
    {
      Serial.println("🔘 Manual button → V1→V2→V3 sequence starting");
      Serial.println("═══════════════════════════════════════════════════════════");
      manualState = MANUAL_V1;
      manualStepStart = millis();
      currentSignal = 100;
      switchPhase2Sent = false;
      sendSignal(100);
    }
    else
    {
      Serial.println("🔘 Manual button → STOP (motor off, valves closed)");
      Serial.println("═══════════════════════════════════════════════════════════");
      currentSignal = 0;
      manualState = MANUAL_IDLE;
      stopAll();
    }
  }
}

// ─── Manual sequence runner ──────────────────────────────────────────────────

void runManualSequence()
{
  if (manualState == MANUAL_IDLE)
    return;

  if (manualState == MANUAL_DONE)
  {
    manualState = MANUAL_IDLE;
    Serial.println("✅ Manual sequence complete");
    Serial.println("═══════════════════════════════════════════════════════════");
    return;
  }

  if (waitingForAck || forceOffActive)
    return;

  unsigned long elapsed = millis() - manualStepStart;

  switch (manualState)
  {

  case MANUAL_V1:
    if (elapsed >= (MANUAL_DURATION - 1000))
    { // Switch 1 second early

      Serial.println("Manual: Switching directly from V1 to V2");

      manualStepStart = millis();
      currentSignal = 10;
      manualState = MANUAL_V2;

      sendSignal(10);
    }
    break;
    // case MANUAL_SWITCH_V2:
    //   // FIX 3: advance on confirmed close (lastAck==0) OR after timeout
    //   if (!switchPhase2Sent &&
    //       (lastAck == 0 || millis() - manualStepStart > SWITCH_TIMEOUT_MS)) {
    //     if (lastAck != 0) {
    //       Serial.println("⚠️  SWITCH_V2: V1 close not confirmed — proceeding anyway");
    //     }
    //     switchPhase2Sent = true;
    //     manualStepStart  = millis();
    //     manualState      = MANUAL_V2;
    //     currentSignal    = 10;
    //     Serial.println("Manual: V1 closed → opening V2");
    //     sendSignal(10);
    //   }
    //   break;

  case MANUAL_V2:
    if (elapsed >= (MANUAL_DURATION - 1000))
    { // Switch 1 second early

      Serial.println("Manual: Switching directly from V2 to V3");

      manualStepStart = millis();
      currentSignal = 1;
      manualState = MANUAL_V3;

      sendSignal(1);
    }
    break;

    // case MANUAL_SWITCH_V3:
    //   // FIX 3: same pattern
    //   if (!switchPhase2Sent &&
    //       (lastAck == 0 || millis() - manualStepStart > SWITCH_TIMEOUT_MS)) {
    //     if (lastAck != 0) {
    //       Serial.println("⚠️  SWITCH_V3: V2 close not confirmed — proceeding anyway");
    //     }
    //     switchPhase2Sent = true;
    //     manualStepStart  = millis();
    //     manualState      = MANUAL_V3;
    //     currentSignal    = 1;
    //     Serial.println("Manual: V2 closed → opening V3");
    //     sendSignal(1);
    //   }
    //   break;

  case MANUAL_V3:
    if (elapsed >= MANUAL_DURATION)
    {
      Serial.println("Manual: V3 time done → motor OFF → closing V3");
      currentSignal = 0;
      manualState = MANUAL_DONE;
      stopAll();
    }
    break;

  default:
    break;
  }
}

// ─── Status printer ──────────────────────────────────────────────────────────

void printStatus()
{
  static unsigned long last = 0;
  if (millis() - last < 10000)
    return;
  last = millis();

  const char *modeStr = (manualState != MANUAL_IDLE) ? "MANUAL" : "SCHEDULE";

  Serial.println(F("\n──────────────────────────────────────"));

  if (rtcAvailable)
  {
    DateTime now = rtc.now();
    Serial.printf("🕐 Time     : %02d:%02d:%02d\n",
                  now.hour(), now.minute(), now.second());
  }
  else
  {
    Serial.println("🕐 Time     : RTC unavailable");
  }

  Serial.printf("⚙️  Mode     : %s\n", modeStr);
  Serial.printf("🔌 Motor    : %s\n", motorRunning ? "ON" : "OFF");
  Serial.printf("📡 Signal   : %03d\n", currentSignal);
  Serial.printf("📥 Last ACK : %03d\n", lastAck);
  Serial.printf("🔁 ForceOff : %s\n", forceOffActive ? "ACTIVE" : "idle");
  Serial.printf("📋 ACK Queue: %d pending\n", ackQueueIdx);

  if (rtcAvailable)
  {
    for (int i = 0; i < 3; i++)
    {
      if (schedules[i].active)
      {
        long remaining = (long)(scheduleStopAt[i] - millis()) / 1000L;
        if (remaining < 0)
          remaining = 0;
        Serial.printf("⏳ Schedule %d : RUNNING — %ld sec left\n", i, remaining);
      }
      else
      {
        Serial.printf("💤 Schedule %d : idle (%02d:%02d, %ds)\n",
                      i, schedules[i].startHour, schedules[i].startMin,
                      schedules[i].durationSec);
      }
    }
  }
  else
  {
    Serial.println("💤 Schedules: DISABLED (RTC unavailable)");
  }

  if (manualState != MANUAL_IDLE && manualState != MANUAL_DONE)
  {
    long remaining = (long)(MANUAL_DURATION - (millis() - manualStepStart)) / 1000L;
    if (remaining < 0)
      remaining = 0;
    const char *stepName[] = {"", "V1", "->V2", "V2", "->V3", "V3", "Done"};
    Serial.printf("🖐  Manual   : step=%s, %ld sec left\n",
                  stepName[(int)manualState], remaining);
  }
  Serial.println(F("──────────────────────────────────────\n"));
}

// ─── Setup ───────────────────────────────────────────────────────────────────

void setup()
{
  Serial.begin(115200);
  Serial.println(F("\n🌱 Smart Sprinkler — Motor Board booting..."));

  pinMode(MOTOR_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(MOTOR_PIN, LOW);

  Wire.begin();
  if (!rtc.begin())
  {
    Serial.println(F("❌ RTC not found — schedules disabled"));
    rtcAvailable = false;
  }
  else if (!rtc.isrunning())
  {
    Serial.println(F("⚠️  RTC not set — setting compile time"));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    rtcAvailable = true;
    DateTime now = rtc.now();
    Serial.printf("🕐 RTC initialized to: %02d:%02d:%02d\n",
                  now.hour(), now.minute(), now.second());
  }
  else
  {
    DateTime now = rtc.now();
    Serial.printf("🕐 RTC time: %02d:%02d:%02d\n",
                  now.hour(), now.minute(), now.second());
    rtcAvailable = true;
  }

  WiFi.mode(WIFI_STA);
  wifi_set_channel(1);
  Serial.print(F("Motor MAC: "));
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0)
  {
    Serial.println(F("❌ ESP-NOW init failed"));
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onReceive);
  esp_now_add_peer(valve1Mac, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);

  lastHeartbeatAt = millis();
  Serial.println(F("✅ Motor Board ready\n"));
}

// ─── Loop ────────────────────────────────────────────────────────────────────

void loop()
{
  handleQueuedAcks();
  handleManualButton();
  runManualSequence();
  checkSchedules();
  retryIfNoAck();
  handleForceOff();
  // handleHeartbeat();
  printStatus();
  delay(50);
}